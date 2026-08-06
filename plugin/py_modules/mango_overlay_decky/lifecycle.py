from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Iterator, Protocol, Sequence


PLUGIN_NAME = "Mango Overlay Decky"
STATE_SCHEMA = 1
MANIFEST_SCHEMA = 1
UNINSTALL_GRACE_SECONDS = 60.0
_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}")
_GENERATION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{7,127}")
_TOKEN = re.compile(r"[a-f0-9]{32,64}")
_PATH_PART = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}")


def _plugin_directory_is_trusted(info: os.stat_result, uid: int) -> bool:
    return (
        stat.S_ISDIR(info.st_mode)
        and info.st_uid in (0, uid)
        and not info.st_mode & 0o022
    )


def _plugin_file_is_trusted(
    info: os.stat_result, uid: int, maximum_size: int
) -> bool:
    return (
        stat.S_ISREG(info.st_mode)
        and info.st_uid in (0, uid)
        and info.st_size <= maximum_size
        and not info.st_mode & 0o022
    )


class LifecycleError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class CommandResult(Protocol):
    returncode: int
    stdout: str
    stderr: str


CommandRunner = Callable[[list[str]], CommandResult]
RuntimeVerifier = Callable[[Path, str], None]


@dataclass(frozen=True)
class LifecycleState:
    active_version: str | None
    previous_version: str | None
    generation: str
    active_runtime: str | None = None
    previous_runtime: str | None = None


@dataclass(frozen=True)
class PreparedRuntime:
    path: Path
    runtime_id: str
    created: bool


@dataclass(frozen=True)
class LifecyclePaths:
    home: Path
    data_root: Path
    versions: Path
    state_root: Path
    state_file: Path
    transaction_file: Path
    pending_uninstall_file: Path
    lock_file: Path
    libexec_root: Path
    config_root: Path
    cache_root: Path
    systemd_user_root: Path
    mangoapp_dropin: Path

    @classmethod
    def for_home(cls, home: Path) -> "LifecyclePaths":
        home = home.absolute()
        data_root = home / ".local/share/mango-overlay-decky"
        state_root = home / ".local/state/mango-overlay-decky"
        systemd_user_root = home / ".config/systemd/user"
        return cls(
            home=home,
            data_root=data_root,
            versions=data_root / "runtime/versions",
            state_root=state_root,
            state_file=state_root / "install.json",
            transaction_file=state_root / "transaction.json",
            pending_uninstall_file=state_root / "pending-uninstall.json",
            lock_file=state_root / "lifecycle.lock",
            libexec_root=home / ".local/libexec/mango-overlay-decky",
            config_root=home / ".config/mango-overlay-decky",
            cache_root=home / ".cache/mango-overlay-decky",
            systemd_user_root=systemd_user_root,
            mangoapp_dropin=(
                systemd_user_root
                / "gamescope-mangoapp.service.d/50-mango-overlay-decky.conf"
            ),
        )


def _default_run(command: list[str]) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    runtime_directory = f"/run/user/{os.geteuid()}"
    environment["XDG_RUNTIME_DIR"] = runtime_directory
    environment["DBUS_SESSION_BUS_ADDRESS"] = (
        f"unix:path={runtime_directory}/bus"
    )
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=15,
        env=environment,
    )


def _elf_identity(path: Path) -> tuple[int, int]:
    try:
        with path.open("rb") as source:
            header = source.read(20)
    except OSError as error:
        raise LifecycleError(
            "desktop_runtime_invalid", f"Could not read desktop runtime file: {path}"
        ) from error
    if len(header) != 20 or header[:4] != b"\x7fELF":
        raise LifecycleError(
            "desktop_runtime_invalid", f"Desktop runtime file is not ELF: {path}"
        )
    elf_class = header[4]
    encoding = header[5]
    if elf_class not in (1, 2) or encoding not in (1, 2):
        raise LifecycleError(
            "desktop_runtime_invalid", f"Desktop runtime ELF header is invalid: {path}"
        )
    byteorder = "little" if encoding == 1 else "big"
    return elf_class, int.from_bytes(header[18:20], byteorder=byteorder)


def _verify_desktop_runtime(runtime: Path) -> None:
    architectures = (
        (
            runtime / "lib",
            (2, 62),
            (
                "libMangoHud.so",
                "libMangoHud_opengl.so",
                "libMangoHud_shim.so",
                "libgif.so.7",
            ),
        ),
        (
            runtime / "lib32",
            (1, 3),
            (
                "libMangoHud.so",
                "libMangoHud_opengl.so",
                "libMangoHud_shim.so",
                "libgif.so.7",
                "libsharpyuv.so.0",
                "libwebp.so.7",
            ),
        ),
    )
    for directory, expected, names in architectures:
        for name in names:
            path = directory / name
            if _elf_identity(path) != expected:
                raise LifecycleError(
                    "desktop_runtime_invalid",
                    f"Desktop runtime file has the wrong ABI: {path}",
                )

    manifest_directory = runtime / "share/vulkan/implicit_layer.d"
    manifests = (
        (
            manifest_directory / "MangoOverlay.x86_64.json",
            "VK_LAYER_MANGOHUD_overlay_x86_64",
            "../../../lib/libMangoHud.so",
            runtime / "lib/libMangoHud.so",
        ),
        (
            manifest_directory / "MangoOverlay.x86.json",
            "VK_LAYER_MANGOHUD_overlay_x86",
            "../../../lib32/libMangoHud.so",
            runtime / "lib32/libMangoHud.so",
        ),
    )
    for path, expected_name, expected_library, expected_target in manifests:
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
            layer = manifest.get("layer") if isinstance(manifest, dict) else None
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise LifecycleError(
                "desktop_runtime_invalid",
                f"Desktop Vulkan manifest is invalid: {path}",
            ) from error
        if (
            not isinstance(manifest, dict)
            or manifest.get("file_format_version") != "1.0.0"
            or not isinstance(layer, dict)
            or layer.get("name") != expected_name
            or layer.get("library_path") != expected_library
            or (path.parent / expected_library).resolve()
            != expected_target.resolve()
        ):
            raise LifecycleError(
                "desktop_runtime_invalid",
                f"Desktop Vulkan manifest targets the wrong library: {path}",
            )


def _default_verify_runtime(runtime: Path, version: str) -> None:
    environment = os.environ.copy()
    library_path = runtime / "lib"
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        part
        for part in (str(library_path), environment.get("LD_LIBRARY_PATH", ""))
        if part
    )
    for name in (
        "mangoapp",
        "mango-overlayd",
        "mango-overlayctl",
        "mango-overlay-test-provider",
    ):
        binary = runtime / "bin" / name
        try:
            result = subprocess.run(
                [str(binary), "--mango-overlay-self-test"],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
                env=environment,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise LifecycleError(
                "runtime_self_test_failed", f"{name} self-test failed: {error}"
            ) from error
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise LifecycleError(
                "runtime_self_test_failed",
                f"{name} self-test returned {result.returncode}: {detail}",
            )


class LifecycleManager:
    def __init__(
        self,
        paths: LifecyclePaths,
        *,
        run_command: CommandRunner = _default_run,
        verify_runtime: RuntimeVerifier = _default_verify_runtime,
        now: Callable[[], float] = time.time,
        token_factory: Callable[[], str] = lambda: secrets.token_hex(16),
    ) -> None:
        self.paths = paths
        self._run_command = run_command
        self._verify_runtime = verify_runtime
        self._now = now
        self._token_factory = token_factory
        self._uid = os.geteuid()

    def status(self) -> LifecycleState:
        state = self._read_state()
        return state or LifecycleState(None, None, "", None, None)

    def test_provider_active(self) -> bool:
        return self._unit_active("mango-overlay-test-provider.service")

    def set_test_provider(self, enabled: bool) -> None:
        state = self._read_state()
        if state is None or state.active_version is None:
            raise LifecycleError("not_installed", "No active runtime is installed")
        self._checked_systemctl(
            "start" if enabled else "stop",
            "mango-overlay-test-provider.service",
        )

    def restart_broker(self) -> None:
        state = self._read_state()
        if state is None or state.active_version is None:
            raise LifecycleError("not_installed", "No active runtime is installed")
        self._checked_systemctl("restart", "mango-overlayd.service")

    def restore_system_mangoapp(self) -> None:
        with self._locked():
            gamescope_was_active = self._gamescope_active()
            self._remove_integration_path(self.paths.mangoapp_dropin)
            self._checked_systemctl("daemon-reload")
            if gamescope_was_active:
                self._checked_systemctl("restart", "gamescope-mangoapp.service")
                if not self._gamescope_active():
                    raise LifecycleError(
                        "system_mangoapp_not_ready",
                        "System MangoApp did not return after restoring its unit",
                    )

    def activate(
        self,
        plugin_root: Path,
        version: str,
        generation: str,
    ) -> LifecycleState:
        self._validate_version(version)
        self._validate_generation(generation)
        plugin_root = plugin_root.absolute()
        self._validate_plugin(plugin_root, version)

        with self._locked():
            self._recover_incomplete_locked()
            prior = self._read_state()
            prepared: PreparedRuntime | None = None
            try:
                prepared = self._prepare_runtime(plugin_root, version)
                self._verify_runtime(prepared.path, version)
            except Exception:
                if prepared is not None and prepared.created:
                    self._remove_owned_tree(prepared.path)
                raise
            runtime_id = prepared.runtime_id

            integration_snapshot = self._capture_integration_files()
            same_runtime = (
                prior is not None and prior.active_runtime == runtime_id
            )
            gamescope_was_active = not same_runtime and self._gamescope_active()
            test_provider_was_active = not same_runtime and self.test_provider_active()
            transaction = {
                "schema": STATE_SCHEMA,
                "candidate": runtime_id,
                "prior": (
                    {"schema": STATE_SCHEMA, **asdict(prior)}
                    if prior is not None
                    else None
                ),
                "integration": integration_snapshot,
            }

            try:
                self._write_json(self.paths.transaction_file, transaction)
                integration_changed = self._install_integration_files(plugin_root)
                state = LifecycleState(
                    active_version=version,
                    active_runtime=runtime_id,
                    previous_version=(
                        prior.previous_version
                        if same_runtime
                        else prior.active_version if prior is not None else None
                    ),
                    previous_runtime=(
                        prior.previous_runtime
                        if same_runtime
                        else prior.active_runtime if prior is not None else None
                    ),
                    generation=generation,
                )
                self._write_state(state)
                if integration_changed:
                    self._reload_and_enable_integration()
                if not same_runtime:
                    self._checked_systemctl("try-restart", "mango-overlayd.service")
                    if test_provider_was_active:
                        self._checked_systemctl(
                            "restart", "mango-overlay-test-provider.service"
                        )
                    if gamescope_was_active:
                        self._checked_systemctl(
                            "restart", "gamescope-mangoapp.service"
                        )
                        if not self._gamescope_active():
                            raise LifecycleError(
                                "mangoapp_not_ready",
                                "gamescope-mangoapp.service did not return to active",
                            )
            except Exception as error:
                self._restore_state(prior)
                self._restore_integration_files(
                    integration_snapshot,
                    prior is not None and prior.active_version is not None,
                )
                self._rollback_running_services(
                    gamescope_was_active, test_provider_was_active
                )
                retained = {
                    retained_version
                    for retained_version in (
                        prior.active_runtime if prior else None,
                        prior.previous_runtime if prior else None,
                    )
                    if retained_version is not None
                }
                if runtime_id not in retained:
                    self._remove_owned_tree(self.paths.versions / runtime_id)
                self._unlink_if_exists(self.paths.transaction_file)
                if isinstance(error, LifecycleError):
                    raise
                raise LifecycleError("activation_failed", str(error)) from error

            self._unlink_if_exists(self.paths.transaction_file)
            self._unlink_if_exists(self.paths.pending_uninstall_file)
            self._prune_versions(state)
            return state

    def mark_pending_uninstall(
        self,
        plugin_root: Path,
        generation: str,
    ) -> str:
        self._validate_generation(generation)
        plugin_root = plugin_root.absolute()
        with self._locked():
            self._recover_incomplete_locked()
            state = self._read_state()
            if state is None or state.active_version is None:
                raise LifecycleError("not_installed", "No active runtime is installed")
            if state.generation != generation:
                raise LifecycleError(
                    "stale_generation",
                    "The uninstall callback does not own the active generation",
                )
            self._validate_plugin(plugin_root, state.active_version)
            token = self._token_factory()
            if _TOKEN.fullmatch(token) is None:
                raise LifecycleError("invalid_token", "Invalid uninstall token")
            self._write_json(
                self.paths.pending_uninstall_file,
                {
                    "schema": STATE_SCHEMA,
                    "token": token,
                    "generation": generation,
                    "plugin_root": str(plugin_root),
                    "created_at": self._now(),
                },
            )
        self._checked_systemctl("start", "mango-overlay-cleanup.timer")
        self._checked_systemctl("start", "mango-overlay-cleanup.service")
        return token

    def finalize_pending_uninstall(self, token: str | None = None) -> bool:
        with self._locked():
            pending = self._read_pending()
            if pending is None:
                return False
            if token is not None and pending["token"] != token:
                return False
            state = self._read_state()
            if state is None or state.generation != pending["generation"]:
                self._unlink_if_exists(self.paths.pending_uninstall_file)
                return False
            plugin_root = Path(pending["plugin_root"])
            if self._path_exists(plugin_root):
                return False
            if self._now() - pending["created_at"] < UNINSTALL_GRACE_SECONDS:
                return False

            cleanup_roots = (
                self.paths.data_root,
                self.paths.libexec_root,
                self.paths.config_root,
                self.paths.cache_root,
                self.paths.state_root,
            )
            gamescope_was_active = self._gamescope_active()
            self._checked_systemctl("stop", "mango-overlay-test-provider.service")
            self._checked_systemctl("disable", "--now", "mango-overlayd.socket")
            self._checked_systemctl("stop", "mango-overlayd.service")
            self._remove_integration_path(self.paths.mangoapp_dropin)
            self._checked_systemctl("daemon-reload")
            if gamescope_was_active:
                self._checked_systemctl(
                    "restart", "gamescope-mangoapp.service"
                )
                if not self._gamescope_active():
                    raise LifecycleError(
                        "system_mangoapp_not_ready",
                        "System MangoApp did not return after restoring its unit",
                    )

            for root in cleanup_roots:
                self._validate_owned_tree(root)
            self._checked_systemctl(
                "disable", "--now", "mango-overlay-cleanup.timer"
            )
            for unit in self._unit_paths():
                self._remove_integration_path(unit)
            self._checked_systemctl("daemon-reload")

            for root in cleanup_roots:
                self._remove_owned_tree(root)
            return True

    @contextmanager
    def _locked(self) -> Iterator[None]:
        self._ensure_private_directory(self.paths.state_root)
        descriptor = os.open(
            self.paths.lock_file,
            os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != self._uid:
                raise LifecycleError("unsafe_lock", "Lifecycle lock is not owned")
            os.fchmod(descriptor, 0o600)
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            yield
        finally:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)

    def _prepare_runtime(
        self, plugin_root: Path, version: str
    ) -> PreparedRuntime:
        source = plugin_root / "runtime"
        manifest_path = source / "manifest.json"
        manifest = self._read_json(manifest_path, 1024 * 1024)
        if (
            manifest.get("schema") != MANIFEST_SCHEMA
            or manifest.get("version") != version
            or not isinstance(manifest.get("files"), list)
        ):
            raise LifecycleError("invalid_manifest", "Runtime manifest is invalid")
        entries = self._validate_manifest_entries(manifest["files"])
        required = {
            "bin/mangoapp",
            "bin/mango-overlayd",
            "bin/mango-overlayctl",
            "bin/mango-overlay-test-provider",
        }
        if not required.issubset(entries):
            raise LifecycleError(
                "invalid_manifest", "Runtime manifest is missing required binaries"
            )
        runtime_id = self._runtime_id(version, entries)

        self._ensure_private_directory(self.paths.data_root)
        self._ensure_private_directory(self.paths.versions)
        destination = self.paths.versions / runtime_id
        if self._path_exists(destination):
            self._verify_runtime_directory(destination, version, entries)
            return PreparedRuntime(destination, runtime_id, False)

        staging = self.paths.versions / (
            f".staging-{runtime_id}-{secrets.token_hex(8)}"
        )
        staging.mkdir(mode=0o700)
        try:
            for relative, entry in entries.items():
                self._copy_verified_file(
                    source / relative,
                    staging / relative,
                    entry["sha256"],
                    entry["mode"],
                )
            self._atomic_write(
                staging / "manifest.json",
                json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode(),
                0o600,
            )
            self._verify_runtime_directory(staging, version, entries)
            os.rename(staging, destination)
        except Exception:
            self._remove_owned_tree(staging)
            raise
        return PreparedRuntime(destination, runtime_id, True)

    @staticmethod
    def _runtime_id(
        version: str,
        entries: dict[str, dict[str, object]],
    ) -> str:
        canonical = {
            "schema": MANIFEST_SCHEMA,
            "version": version,
            "files": [
                {
                    "path": relative,
                    "sha256": entries[relative]["sha256"],
                    "mode": entries[relative]["mode"],
                }
                for relative in sorted(entries)
            ],
        }
        return hashlib.sha256(
            json.dumps(canonical, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()

    def _validate_manifest_entries(
        self, raw_entries: list[object]
    ) -> dict[str, dict[str, object]]:
        entries: dict[str, dict[str, object]] = {}
        total = 0
        for raw in raw_entries:
            if not isinstance(raw, dict):
                raise LifecycleError("invalid_manifest", "Invalid manifest entry")
            relative = raw.get("path")
            digest = raw.get("sha256")
            mode = raw.get("mode")
            if (
                not isinstance(relative, str)
                or not isinstance(digest, str)
                or re.fullmatch(r"[a-f0-9]{64}", digest) is None
                or not isinstance(mode, int)
                or mode < 0o400
                or mode > 0o755
                or mode & 0o022
            ):
                raise LifecycleError("invalid_manifest", "Invalid manifest fields")
            path = PurePosixPath(relative)
            if (
                path.is_absolute()
                or not path.parts
                or any(_PATH_PART.fullmatch(part) is None for part in path.parts)
                or relative in entries
            ):
                raise LifecycleError("invalid_manifest", "Unsafe manifest path")
            entries[relative] = {
                "sha256": digest,
                "mode": mode,
            }
            total += 1
        if total == 0 or total > 256:
            raise LifecycleError("invalid_manifest", "Invalid manifest file count")
        return entries

    def _verify_runtime_directory(
        self,
        runtime: Path,
        version: str,
        entries: dict[str, dict[str, object]],
    ) -> None:
        self._validate_owned_directory(runtime)
        installed_manifest = self._read_json(runtime / "manifest.json", 1024 * 1024)
        if installed_manifest.get("version") != version:
            raise LifecycleError("runtime_mismatch", "Installed runtime version differs")
        expected = {"manifest.json", *entries.keys()}
        actual: set[str] = set()
        for path in runtime.rglob("*"):
            relative = path.relative_to(runtime).as_posix()
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or info.st_uid != self._uid:
                raise LifecycleError("unsafe_runtime", "Runtime contains an unsafe path")
            if stat.S_ISREG(info.st_mode):
                actual.add(relative)
        if actual != expected:
            raise LifecycleError("runtime_mismatch", "Runtime file set differs")
        for relative, entry in entries.items():
            path = runtime / relative
            if self._sha256(path) != entry["sha256"]:
                raise LifecycleError("runtime_mismatch", "Runtime digest differs")
            if stat.S_IMODE(path.stat().st_mode) != entry["mode"]:
                raise LifecycleError("runtime_mismatch", "Runtime mode differs")

    def _install_integration_files(self, plugin_root: Path) -> bool:
        self._ensure_private_directory(self.paths.libexec_root)
        source_root = plugin_root / "py_modules/mango_overlay_decky"
        changed = False
        for name in ("lifecycle.py", "launcher.py"):
            changed |= self._copy_if_changed(
                source_root / name,
                self.paths.libexec_root / name,
                0o755,
            )

        launcher = self.paths.libexec_root / "launcher.py"
        lifecycle = self.paths.libexec_root / "lifecycle.py"
        units = {
            self.paths.systemd_user_root / "mango-overlayd.socket": (
                "[Unit]\nDescription=Mango Overlay scene broker socket\n\n"
                "[Socket]\nListenSequentialPacket=%t/mango-overlay-decky.sock\n"
                "SocketMode=0600\nRemoveOnStop=yes\n\n"
                "[Install]\nWantedBy=sockets.target\n"
            ),
            self.paths.systemd_user_root / "mango-overlayd.service": (
                "[Unit]\nDescription=Mango Overlay scene broker\n"
                "Requires=mango-overlayd.socket\nAfter=mango-overlayd.socket\n\n"
                "[Service]\nType=simple\n"
                f'ExecStart="{launcher}" broker\n'
                "Restart=on-failure\nRestartSec=1\nNoNewPrivileges=yes\n"
            ),
            self.paths.systemd_user_root / "mango-overlay-cleanup.service": (
                "[Unit]\nDescription=Finalize Mango Overlay Decky uninstall\n\n"
                "[Service]\nType=oneshot\n"
                f'ExecStart=/usr/bin/python3 "{lifecycle}" finalize '
                f'--home "{self.paths.home}"\n'
            ),
            self.paths.systemd_user_root / "mango-overlay-cleanup.timer": (
                "[Unit]\nDescription=Check Mango Overlay Decky uninstall state\n\n"
                "[Timer]\nOnBootSec=60s\nOnUnitActiveSec=60s\n"
                "Unit=mango-overlay-cleanup.service\n\n"
                "[Install]\nWantedBy=timers.target\n"
            ),
            self.paths.systemd_user_root / "mango-overlay-test-provider.service": (
                "[Unit]\nDescription=Mango Overlay test canvas\n"
                "Requires=mango-overlayd.socket\nAfter=mango-overlayd.socket\n\n"
                "[Service]\nType=simple\n"
                f'ExecStart="{launcher}" test-provider\n'
                "Restart=on-failure\nRestartSec=1\nNoNewPrivileges=yes\n"
            ),
            self.paths.mangoapp_dropin: (
                "[Service]\nExecStart=\n"
                f'ExecStart="{launcher}" mangoapp\n'
            ),
        }
        for path, contents in units.items():
            changed |= self._write_if_changed(path, contents.encode(), 0o644)
        return changed

    def _integration_paths(self) -> dict[str, Path]:
        return {
            "lifecycle": self.paths.libexec_root / "lifecycle.py",
            "launcher": self.paths.libexec_root / "launcher.py",
            "broker_socket": self.paths.systemd_user_root / "mango-overlayd.socket",
            "broker_service": self.paths.systemd_user_root / "mango-overlayd.service",
            "cleanup_service": (
                self.paths.systemd_user_root / "mango-overlay-cleanup.service"
            ),
            "cleanup_timer": (
                self.paths.systemd_user_root / "mango-overlay-cleanup.timer"
            ),
            "test_provider_service": (
                self.paths.systemd_user_root
                / "mango-overlay-test-provider.service"
            ),
            "mangoapp_dropin": self.paths.mangoapp_dropin,
        }

    def _capture_integration_files(self) -> dict[str, object]:
        snapshot: dict[str, object] = {}
        total_size = 0
        for name, path in self._integration_paths().items():
            try:
                info = path.lstat()
            except FileNotFoundError:
                snapshot[name] = None
                continue
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != self._uid
                or info.st_mode & 0o022
                or info.st_size > 512 * 1024
            ):
                raise LifecycleError(
                    "unsafe_integration", f"Unsafe integration file: {path}"
                )
            contents = path.read_bytes()
            total_size += len(contents)
            if total_size > 1024 * 1024:
                raise LifecycleError(
                    "unsafe_integration", "Integration snapshot is too large"
                )
            snapshot[name] = {
                "mode": stat.S_IMODE(info.st_mode),
                "contents": base64.b64encode(contents).decode("ascii"),
            }
        return snapshot

    def _decode_integration_snapshot(
        self, raw: object
    ) -> dict[Path, tuple[bytes, int] | None]:
        paths = self._integration_paths()
        if not isinstance(raw, dict) or set(raw) != set(paths):
            raise LifecycleError(
                "invalid_transaction", "Integration snapshot is invalid"
            )
        decoded: dict[Path, tuple[bytes, int] | None] = {}
        total_size = 0
        for name, path in paths.items():
            entry = raw[name]
            if entry is None:
                decoded[path] = None
                continue
            if not isinstance(entry, dict):
                raise LifecycleError(
                    "invalid_transaction", "Integration snapshot entry is invalid"
                )
            mode = entry.get("mode")
            encoded = entry.get("contents")
            if (
                not isinstance(mode, int)
                or mode < 0o400
                or mode > 0o755
                or mode & 0o022
                or not isinstance(encoded, str)
            ):
                raise LifecycleError(
                    "invalid_transaction", "Integration snapshot metadata is invalid"
                )
            try:
                contents = base64.b64decode(encoded, validate=True)
            except (ValueError, TypeError) as error:
                raise LifecycleError(
                    "invalid_transaction", "Integration snapshot data is invalid"
                ) from error
            total_size += len(contents)
            if len(contents) > 512 * 1024 or total_size > 1024 * 1024:
                raise LifecycleError(
                    "invalid_transaction", "Integration snapshot is too large"
                )
            decoded[path] = (contents, mode)
        return decoded

    def _restore_integration_files(
        self, raw_snapshot: object, prior_installed: bool
    ) -> None:
        snapshot = self._decode_integration_snapshot(raw_snapshot)
        current_integration_exists = any(
            self._path_exists(path) for path in snapshot
        )
        if not prior_installed and current_integration_exists:
            self._checked_systemctl(
                "disable",
                "--now",
                "mango-overlayd.socket",
                "mango-overlay-cleanup.timer",
            )
            self._checked_systemctl("stop", "mango-overlay-test-provider.service")
            self._checked_systemctl("stop", "mango-overlayd.service")
        for path, entry in snapshot.items():
            if entry is None:
                self._remove_integration_path(path)
            else:
                contents, mode = entry
                self._atomic_write(path, contents, mode)
        if current_integration_exists or prior_installed:
            if prior_installed:
                self._reload_and_enable_integration()
            else:
                self._checked_systemctl("daemon-reload")

    def _reload_and_enable_integration(self) -> None:
        self._checked_systemctl("daemon-reload")
        self._checked_systemctl(
            "enable",
            "--now",
            "mango-overlayd.socket",
            "mango-overlay-cleanup.timer",
        )

    def _rollback_running_services(
        self,
        gamescope_was_active: bool,
        test_provider_was_active: bool,
    ) -> None:
        try:
            self._checked_systemctl("try-restart", "mango-overlayd.service")
            if test_provider_was_active:
                self._checked_systemctl(
                    "restart", "mango-overlay-test-provider.service"
                )
            if gamescope_was_active:
                self._checked_systemctl(
                    "restart", "gamescope-mangoapp.service"
                )
        except LifecycleError:
            pass

    def _recover_incomplete_locked(self) -> None:
        if not self._path_exists(self.paths.transaction_file):
            return
        transaction = self._read_json(self.paths.transaction_file, 2 * 1024 * 1024)
        if transaction.get("schema") != STATE_SCHEMA:
            raise LifecycleError("invalid_transaction", "Transaction state is invalid")
        prior_raw = transaction.get("prior")
        prior = self._decode_state(prior_raw) if prior_raw is not None else None
        prior_installed = prior is not None and prior.active_version is not None
        self._restore_state(prior)
        self._restore_integration_files(
            transaction.get("integration"),
            prior_installed,
        )
        candidate = transaction.get("candidate")
        gamescope_was_active = self._gamescope_active()
        if prior_installed:
            self._checked_systemctl("try-restart", "mango-overlayd.service")
        if gamescope_was_active:
            self._checked_systemctl("restart", "gamescope-mangoapp.service")
        self._unlink_if_exists(self.paths.transaction_file)
        retained = {
            runtime_id
            for runtime_id in (
                prior.active_runtime if prior else None,
                prior.previous_runtime if prior else None,
            )
            if runtime_id is not None
        }
        if isinstance(candidate, str) and candidate not in retained:
            self._remove_owned_tree(self.paths.versions / candidate)

    def _prune_versions(self, state: LifecycleState) -> None:
        retained = {
            runtime_id
            for runtime_id in (state.active_runtime, state.previous_runtime)
            if runtime_id is not None
        }
        if not self._path_exists(self.paths.versions):
            return
        for path in self.paths.versions.iterdir():
            if path.name in retained:
                continue
            if _VERSION.fullmatch(path.name) is not None:
                self._remove_owned_tree(path)

    def _validate_plugin(self, plugin_root: Path, version: str) -> None:
        try:
            info = plugin_root.lstat()
        except FileNotFoundError as error:
            raise LifecycleError("invalid_plugin", "Decky plugin root is missing") from error
        if not _plugin_directory_is_trusted(info, self._uid):
            raise LifecycleError("invalid_plugin", "Decky plugin root is unsafe")
        plugin_manifest = self._read_plugin_json(
            plugin_root / "plugin.json", 65536
        )
        package_manifest = self._read_plugin_json(
            plugin_root / "package.json", 65536
        )
        if (
            plugin_manifest.get("name") != PLUGIN_NAME
            or package_manifest.get("version") != version
        ):
            raise LifecycleError("invalid_plugin", "Decky plugin identity differs")

    def _read_state(self) -> LifecycleState | None:
        if not self._path_exists(self.paths.state_file):
            return None
        return self._decode_state(self._read_json(self.paths.state_file, 65536))

    def _decode_state(self, raw: object) -> LifecycleState:
        if not isinstance(raw, dict) or raw.get("schema") != STATE_SCHEMA:
            raise LifecycleError("invalid_state", "Installation state is invalid")
        active = raw.get("active_version")
        previous = raw.get("previous_version")
        active_runtime = raw.get("active_runtime", active)
        previous_runtime = raw.get("previous_runtime", previous)
        generation = raw.get("generation")
        if active is not None:
            self._validate_version(active)
        if previous is not None:
            self._validate_version(previous)
        if (active is None) != (active_runtime is None) or (
            previous is None
        ) != (previous_runtime is None):
            raise LifecycleError("invalid_state", "Runtime state is inconsistent")
        if active_runtime is not None:
            self._validate_version(active_runtime)
        if previous_runtime is not None:
            self._validate_version(previous_runtime)
        if not isinstance(generation, str):
            raise LifecycleError("invalid_state", "Installation generation is invalid")
        self._validate_generation(generation)
        return LifecycleState(
            active,
            previous,
            generation,
            active_runtime,
            previous_runtime,
        )

    def _write_state(self, state: LifecycleState) -> None:
        self._write_json(
            self.paths.state_file,
            {"schema": STATE_SCHEMA, **asdict(state)},
        )

    def _restore_state(self, state: LifecycleState | None) -> None:
        if state is None:
            self._unlink_if_exists(self.paths.state_file)
        else:
            self._write_state(state)

    def _read_pending(self) -> dict[str, object] | None:
        if not self._path_exists(self.paths.pending_uninstall_file):
            return None
        raw = self._read_json(self.paths.pending_uninstall_file, 65536)
        token = raw.get("token")
        generation = raw.get("generation")
        plugin_root = raw.get("plugin_root")
        created_at = raw.get("created_at")
        if (
            raw.get("schema") != STATE_SCHEMA
            or not isinstance(token, str)
            or _TOKEN.fullmatch(token) is None
            or not isinstance(generation, str)
            or _GENERATION.fullmatch(generation) is None
            or not isinstance(plugin_root, str)
            or not Path(plugin_root).is_absolute()
            or not isinstance(created_at, (int, float))
        ):
            raise LifecycleError("invalid_pending", "Pending uninstall state is invalid")
        return raw

    def _gamescope_active(self) -> bool:
        return self._unit_active("gamescope-mangoapp.service")

    def _unit_active(self, unit: str) -> bool:
        result = self._run_command(
            [
                "systemctl",
                "--user",
                "is-active",
                "--quiet",
                unit,
            ]
        )
        return result.returncode == 0

    def _checked_systemctl(self, *arguments: str) -> None:
        command = ["systemctl", "--user", *arguments]
        result = self._run_command(command)
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise LifecycleError(
                "systemctl_failed",
                f"{' '.join(command)} failed: {detail}",
            )

    def _unit_paths(self) -> tuple[Path, ...]:
        return (
            self.paths.systemd_user_root / "mango-overlayd.socket",
            self.paths.systemd_user_root / "mango-overlayd.service",
            self.paths.systemd_user_root / "mango-overlay-cleanup.service",
            self.paths.systemd_user_root / "mango-overlay-cleanup.timer",
            self.paths.systemd_user_root / "mango-overlay-test-provider.service",
        )

    def _write_json(self, path: Path, value: object) -> None:
        self._atomic_write(
            path,
            json.dumps(value, sort_keys=True, separators=(",", ":")).encode(),
            0o600,
        )

    def _read_json(self, path: Path, maximum_size: int) -> dict[str, object]:
        info = path.lstat()
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != self._uid
            or info.st_size > maximum_size
            or info.st_mode & 0o022
        ):
            raise LifecycleError("unsafe_file", f"Unsafe file: {path}")
        return self._decode_json_object(path)

    def _read_plugin_json(
        self, path: Path, maximum_size: int
    ) -> dict[str, object]:
        info = path.lstat()
        if not _plugin_file_is_trusted(info, self._uid, maximum_size):
            raise LifecycleError("unsafe_file", f"Unsafe plugin file: {path}")
        return self._decode_json_object(path)

    @staticmethod
    def _decode_json_object(path: Path) -> dict[str, object]:
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise LifecycleError("invalid_json", f"Invalid JSON: {path}") from error
        if not isinstance(value, dict):
            raise LifecycleError("invalid_json", f"Expected an object: {path}")
        return value

    def _atomic_write(self, path: Path, contents: bytes, mode: int) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.parent / f".{path.name}.{secrets.token_hex(8)}.tmp"
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            mode,
        )
        try:
            with os.fdopen(descriptor, "wb", closefd=False) as output:
                output.write(contents)
                output.flush()
                os.fsync(output.fileno())
            os.fchmod(descriptor, mode)
        finally:
            os.close(descriptor)
        os.replace(temporary, path)

    def _write_if_changed(self, path: Path, contents: bytes, mode: int) -> bool:
        if self._path_exists(path):
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != self._uid:
                raise LifecycleError("unsafe_file", f"Unsafe managed file: {path}")
            if path.read_bytes() == contents and stat.S_IMODE(info.st_mode) == mode:
                return False
        self._atomic_write(path, contents, mode)
        return True

    def _copy_if_changed(self, source: Path, destination: Path, mode: int) -> bool:
        source_info = source.lstat()
        if (
            not stat.S_ISREG(source_info.st_mode)
            or source_info.st_uid != self._uid
            or source_info.st_mode & 0o022
        ):
            raise LifecycleError("unsafe_file", f"Unsafe helper source: {source}")
        contents = source.read_bytes()
        return self._write_if_changed(destination, contents, mode)

    def _copy_verified_file(
        self,
        source: Path,
        destination: Path,
        expected_digest: object,
        mode: object,
    ) -> None:
        if not isinstance(expected_digest, str) or not isinstance(mode, int):
            raise LifecycleError("invalid_manifest", "Invalid file metadata")
        source_info = source.lstat()
        if (
            not stat.S_ISREG(source_info.st_mode)
            or source_info.st_uid != self._uid
            or source_info.st_mode & 0o022
            or source_info.st_size > 256 * 1024 * 1024
        ):
            raise LifecycleError("unsafe_file", f"Unsafe runtime source: {source}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        with source.open("rb") as input_file, destination.open("xb") as output_file:
            while chunk := input_file.read(1024 * 1024):
                digest.update(chunk)
                output_file.write(chunk)
            output_file.flush()
            os.fsync(output_file.fileno())
        if digest.hexdigest() != expected_digest:
            destination.unlink()
            raise LifecycleError("digest_mismatch", f"Digest differs: {source}")
        destination.chmod(mode)

    def _sha256(self, path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as file:
            while chunk := file.read(1024 * 1024):
                digest.update(chunk)
        return digest.hexdigest()

    def _ensure_private_directory(self, path: Path) -> None:
        path.mkdir(parents=True, mode=0o700, exist_ok=True)
        self._validate_owned_directory(path)

    def _validate_owned_directory(self, path: Path) -> None:
        info = path.lstat()
        if (
            not stat.S_ISDIR(info.st_mode)
            or info.st_uid != self._uid
            or info.st_mode & 0o022
        ):
            raise LifecycleError("unsafe_directory", f"Unsafe directory: {path}")

    def _validate_owned_tree(self, root: Path) -> None:
        if not self._path_exists(root):
            return
        root_info = root.lstat()
        if stat.S_ISLNK(root_info.st_mode) or root_info.st_uid != self._uid:
            raise LifecycleError("unsafe_tree", f"Unsafe managed root: {root}")
        for path in root.rglob("*"):
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or info.st_uid != self._uid:
                raise LifecycleError("unsafe_tree", f"Unsafe managed path: {path}")

    def _remove_owned_tree(self, root: Path) -> None:
        if not self._path_exists(root):
            return
        self._validate_owned_tree(root)
        shutil.rmtree(root)

    def _unlink_if_exists(self, path: Path) -> None:
        try:
            info = path.lstat()
        except FileNotFoundError:
            return
        if not stat.S_ISREG(info.st_mode) or info.st_uid != self._uid:
            raise LifecycleError("unsafe_file", f"Unsafe managed file: {path}")
        path.unlink()
        try:
            path.parent.rmdir()
        except OSError:
            pass

    def _remove_integration_path(self, path: Path) -> None:
        try:
            info = path.lstat()
        except FileNotFoundError:
            return
        if info.st_uid != self._uid or not (
            stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode)
        ):
            raise LifecycleError("unsafe_file", f"Unsafe integration file: {path}")
        path.unlink()
        try:
            path.parent.rmdir()
        except OSError:
            pass

    @staticmethod
    def _path_exists(path: Path) -> bool:
        try:
            path.lstat()
            return True
        except FileNotFoundError:
            return False

    @staticmethod
    def _validate_version(version: object) -> None:
        if not isinstance(version, str) or _VERSION.fullmatch(version) is None:
            raise LifecycleError("invalid_version", "Invalid runtime version")

    @staticmethod
    def _validate_generation(generation: object) -> None:
        if (
            not isinstance(generation, str)
            or _GENERATION.fullmatch(generation) is None
        ):
            raise LifecycleError("invalid_generation", "Invalid plugin generation")


def _main() -> int:
    parser = argparse.ArgumentParser(prog="mango-overlay-lifecycle")
    subcommands = parser.add_subparsers(dest="command", required=True)
    finalize = subcommands.add_parser("finalize")
    finalize.add_argument("--home", type=Path, required=True)
    restore = subcommands.add_parser("restore-system")
    restore.add_argument("--home", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        manager = LifecycleManager(LifecyclePaths.for_home(arguments.home))
        if arguments.command == "finalize":
            manager.finalize_pending_uninstall()
        elif arguments.command == "restore-system":
            manager.restore_system_mangoapp()
    except LifecycleError as error:
        print(f"{error.code}: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
