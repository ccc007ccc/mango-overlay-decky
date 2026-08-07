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
from typing import Callable, Iterator, Protocol, Sequence, TYPE_CHECKING

if TYPE_CHECKING:
    from .coordinator import RuntimeCandidate


PLUGIN_NAME = "Mango Overlay Decky"
STATE_SCHEMA = 1
MANIFEST_SCHEMA = 1
UNINSTALL_GRACE_SECONDS = 3.0
MAX_PLUGIN_DIRECTORIES = 256
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


class LifecycleRuntimeOperations:
    """Bridge coordinator activation to the lifecycle transaction seam.

    The coordinator never receives a plugin path or executes a candidate.  A
    product prepares its immutable runtime first, then this adapter resolves
    the bounded content revision through ``LifecycleManager`` and performs the
    one actual activation transaction.
    """

    def __init__(
        self,
        manager: "LifecycleManager",
        *,
        plugin_root: Path | None,
        plugin_version: str | None,
        generation: str,
    ) -> None:
        self.manager = manager
        self.plugin_root = plugin_root
        self.plugin_version = plugin_version
        self.generation = generation

    def activate(self, candidate: "RuntimeCandidate") -> None:
        if candidate.runtime_ref != candidate.content_revision:
            raise LifecycleError(
                "invalid_runtime_ref",
                "Runtime candidate reference is not its content revision",
            )
        self.manager.activate_prepared_runtime(
            candidate.content_revision,
            plugin_root=self.plugin_root,
            version=self.plugin_version,
            generation=self.generation,
            verify=False,
        )

    def ensure_candidate(self, candidate: "RuntimeCandidate") -> None:
        if candidate.runtime_ref != candidate.content_revision:
            raise LifecycleError(
                "invalid_runtime_ref",
                "Runtime candidate reference is not its content revision",
            )
        self.manager.ensure_prepared_runtime(candidate.content_revision)

    def verify_candidate(self, candidate: "RuntimeCandidate") -> None:
        if candidate.runtime_ref != candidate.content_revision:
            raise LifecycleError(
                "invalid_runtime_ref",
                "Runtime candidate reference is not its content revision",
            )
        self.manager.verify_prepared_runtime(candidate.content_revision)

    def restore_system(self) -> None:
        self.manager.restore_system_mangoapp()

    def refresh_generation(self, candidate: "RuntimeCandidate") -> None:
        """Record a same-product reload without restarting the renderer."""

        if candidate.runtime_ref != candidate.content_revision:
            raise LifecycleError(
                "invalid_runtime_ref",
                "Runtime candidate reference is not its content revision",
            )
        self.manager.refresh_active_generation(
            candidate.content_revision,
            self.generation,
        )

    def current_active_revision(self) -> str | None:
        """Expose only the legacy active pointer for coordinator bootstrap."""

        return self.manager.status().active_runtime

    def current_active_candidate(self) -> "RuntimeCandidate" | None:
        """Expose bounded metadata for a legacy active runtime."""

        return self.manager.active_runtime_candidate()


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
    decky_settings_root: Path
    decky_runtime_root: Path
    decky_logs_root: Path
    systemd_user_root: Path
    mangoapp_dropin: Path

    @classmethod
    def for_home(cls, home: Path) -> "LifecyclePaths":
        home = home.absolute()
        data_root = home / ".local/share/mango-overlay-decky"
        state_root = home / ".local/state/mango-overlay-decky"
        decky_root = home / "homebrew"
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
            decky_settings_root=decky_root / "settings/mango-overlay-decky",
            decky_runtime_root=decky_root / "data/mango-overlay-decky",
            decky_logs_root=decky_root / "logs/mango-overlay-decky",
            systemd_user_root=systemd_user_root,
            mangoapp_dropin=(
                systemd_user_root
                / "gamescope-mangoapp.service.d/50-mango-overlay-decky.conf"
            ),
        )


def _command_environment() -> dict[str, str]:
    environment = os.environ.copy()
    runtime_directory = f"/run/user/{os.geteuid()}"
    environment["XDG_RUNTIME_DIR"] = runtime_directory
    environment["DBUS_SESSION_BUS_ADDRESS"] = (
        f"unix:path={runtime_directory}/bus"
    )
    return environment


def _run_command(
    command: list[str], timeout: float
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=_command_environment(),
    )


def _default_run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return _run_command(command, 15)


def _default_run_quick(command: list[str]) -> subprocess.CompletedProcess[str]:
    return _run_command(command, 1)


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
        run_quick_command: CommandRunner = _default_run_quick,
        verify_runtime: RuntimeVerifier = _default_verify_runtime,
        now: Callable[[], float] = time.time,
        token_factory: Callable[[], str] = lambda: secrets.token_hex(16),
    ) -> None:
        self.paths = paths
        self._run_command = run_command
        self._run_quick_command = run_quick_command
        self._verify_runtime = verify_runtime
        self._now = now
        self._token_factory = token_factory
        self._uid = os.geteuid()

    def status(self) -> LifecycleState:
        state = self._read_state()
        return state or LifecycleState(None, None, "", None, None)

    def refresh_active_generation(
        self, runtime_id: str, generation: str
    ) -> LifecycleState:
        """Refresh the owning product generation without changing the runtime."""

        self._validate_generation(generation)
        if not isinstance(runtime_id, str) or re.fullmatch(
            r"[a-f0-9]{64}", runtime_id
        ) is None:
            raise LifecycleError("invalid_runtime", "Runtime revision is invalid")
        with self._locked():
            self._recover_incomplete_locked()
            state = self._read_state()
            if state is None or state.active_runtime != runtime_id:
                raise LifecycleError(
                    "active_runtime_changed",
                    "The shared active runtime changed before generation refresh",
                )
            refreshed = LifecycleState(
                active_version=state.active_version,
                previous_version=state.previous_version,
                generation=generation,
                active_runtime=state.active_runtime,
                previous_runtime=state.previous_runtime,
            )
            self._write_state(refreshed)
            if self._path_exists(self.paths.pending_uninstall_file):
                self._unlink_if_exists(self.paths.pending_uninstall_file)
                self._stop_cleanup_timer()
            return refreshed

    def active_runtime_candidate(self) -> "RuntimeCandidate" | None:
        """Read the active runtime manifest without running its binaries."""

        try:
            from .coordinator import RuntimeCandidate
        except ImportError:  # launcher/lifecycle helpers are also flat files
            from coordinator import RuntimeCandidate  # type: ignore[no-redef]

        state = self.status()
        if state.active_runtime is None:
            return None
        runtime = self.ensure_prepared_runtime(state.active_runtime)
        manifest = self._read_json(runtime / "manifest.json", 1024 * 1024)
        version = manifest.get("version")
        core_version = manifest.get("core_version", version)
        if not isinstance(version, str) or not isinstance(core_version, str):
            raise LifecycleError(
                "invalid_core_version", "Active runtime manifest is invalid"
            )
        return RuntimeCandidate.create(
            core_version=core_version,
            content_revision=state.active_runtime,
            runtime_ref=state.active_runtime,
        )

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

    def prepare_runtime_candidate(
        self,
        plugin_root: Path,
        version: str,
    ) -> "RuntimeCandidate":
        """Copy and verify a product runtime without changing the active one."""

        try:
            from .coordinator import RuntimeCandidate
        except ImportError:  # launcher/lifecycle helpers are also flat files
            from coordinator import RuntimeCandidate  # type: ignore[no-redef]

        self._validate_version(version)
        plugin_root = plugin_root.absolute()
        self._validate_plugin(plugin_root, version)
        with self._locked():
            self._recover_incomplete_locked()
            prepared: PreparedRuntime | None = None
            try:
                prepared = self._prepare_runtime(plugin_root, version)
                manifest = self._read_json(
                    prepared.path / "manifest.json", 1024 * 1024
                )
                core_version = manifest.get("core_version", version)
                if not isinstance(core_version, str):
                    raise LifecycleError(
                        "invalid_core_version", "Runtime manifest core version is invalid"
                    )
                return RuntimeCandidate.create(
                    core_version=core_version,
                    content_revision=prepared.runtime_id,
                    runtime_ref=prepared.runtime_id,
                )
            except Exception:
                if prepared is not None and prepared.created:
                    self._remove_owned_tree(prepared.path)
                raise

    def activate_prepared_runtime(
        self,
        runtime_id: str,
        *,
        plugin_root: Path | None,
        version: str | None,
        generation: str,
        verify: bool = True,
    ) -> LifecycleState:
        """Activate an already prepared immutable revision.

        ``runtime_id`` is deliberately restricted to the content-addressed
        directory name.  ``plugin_root`` is used only to refresh the helper
        files during a product registration; launcher retries can pass
        ``None`` and use the already installed helper directory.
        """

        self._validate_generation(generation)
        if not isinstance(runtime_id, str) or re.fullmatch(r"[a-f0-9]{64}", runtime_id) is None:
            raise LifecycleError("invalid_runtime", "Runtime revision is invalid")
        if plugin_root is not None:
            plugin_root = plugin_root.absolute()
            if version is None:
                raise LifecycleError("invalid_version", "Plugin version is required")
            self._validate_version(version)
            self._validate_plugin(plugin_root, version)
        with self._locked():
            self._recover_incomplete_locked()
            runtime = self.paths.versions / runtime_id
            manifest = self._read_json(runtime / "manifest.json", 1024 * 1024)
            manifest_version = manifest.get("version")
            if not isinstance(manifest_version, str):
                raise LifecycleError("invalid_manifest", "Prepared runtime manifest is invalid")
            entries = self._validate_manifest_entries(manifest.get("files", []))
            self._verify_runtime_directory(runtime, manifest_version, entries)
            if verify:
                self._verify_runtime(runtime, manifest_version)
            activation_version = version or manifest_version
            return self._activate_prepared_locked(
                PreparedRuntime(runtime, runtime_id, False),
                plugin_root,
                activation_version,
                generation,
                retain_candidates=True,
            )

    def ensure_prepared_runtime(self, runtime_id: str) -> Path:
        """Cheap structural validation used for every registered claim."""

        if not isinstance(runtime_id, str) or re.fullmatch(r"[a-f0-9]{64}", runtime_id) is None:
            raise LifecycleError("invalid_runtime", "Runtime revision is invalid")
        runtime = self.paths.versions / runtime_id
        manifest = self._read_json(runtime / "manifest.json", 1024 * 1024)
        version = manifest.get("version")
        if not isinstance(version, str):
            raise LifecycleError("invalid_manifest", "Prepared runtime manifest is invalid")
        entries = self._validate_manifest_entries(manifest.get("files", []))
        self._verify_runtime_directory(runtime, version, entries)
        return runtime

    def verify_prepared_runtime(self, runtime_id: str) -> None:
        """Run the expensive self-test for one structurally valid revision."""

        runtime = self.ensure_prepared_runtime(runtime_id)
        manifest = self._read_json(runtime / "manifest.json", 1024 * 1024)
        version = manifest.get("version")
        if not isinstance(version, str):
            raise LifecycleError("invalid_manifest", "Prepared runtime manifest is invalid")
        self._verify_runtime(runtime, version)

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
            return self._activate_prepared_locked(
                prepared,
                plugin_root,
                version,
                generation,
                retain_candidates=False,
            )

    def _activate_prepared_locked(
        self,
        prepared: PreparedRuntime,
        plugin_root: Path | None,
        version: str,
        generation: str,
        *,
        retain_candidates: bool,
    ) -> LifecycleState:
        """Run one atomic activation transaction; caller holds lifecycle lock."""

        prior = self._read_state()
        runtime_id = prepared.runtime_id
        integration_snapshot = self._capture_integration_files()
        same_runtime = prior is not None and prior.active_runtime == runtime_id
        gamescope_was_active = not same_runtime and self._gamescope_active()
        test_provider_was_active = not same_runtime and self.test_provider_active()
        transaction = {
            "schema": STATE_SCHEMA,
            "candidate": runtime_id,
            "retain_candidate": retain_candidates,
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
            if not retain_candidates and runtime_id not in retained:
                self._remove_owned_tree(self.paths.versions / runtime_id)
            self._unlink_if_exists(self.paths.transaction_file)
            if isinstance(error, LifecycleError):
                raise
            raise LifecycleError("activation_failed", str(error)) from error

        self._unlink_if_exists(self.paths.transaction_file)
        if self._path_exists(self.paths.pending_uninstall_file):
            self._unlink_if_exists(self.paths.pending_uninstall_file)
            self._stop_cleanup_timer()
        if not retain_candidates:
            self._prune_versions(state)
        return state

    def mark_pending_uninstall(
        self,
        plugin_root: Path,
        generation: str,
        *,
        coordinator_token: str | None = None,
        coordinator_product_id: str | None = None,
    ) -> str:
        self._validate_generation(generation)
        plugin_root = plugin_root.absolute()
        coordinated = coordinator_token is not None or coordinator_product_id is not None
        if coordinated and (coordinator_token is None or coordinator_product_id is None):
            raise LifecycleError(
                "invalid_coordinator_claim",
                "Coordinator token and product identity must be supplied together",
            )
        with self._locked():
            self._recover_incomplete_locked()
            state = self._read_state()
            if coordinated:
                # A shared claim may belong to a product whose candidate is
                # retained but is not the currently active revision.  Its
                # uninstall record must still be durable so the coordinator
                # can remove exactly that claim later.
                self._validate_pending_plugin_root(plugin_root)
            else:
                if state is None or state.active_version is None:
                    raise LifecycleError("not_installed", "No active runtime is installed")
                if state.generation != generation:
                    raise LifecycleError(
                        "stale_generation",
                        "The uninstall callback does not own the active generation",
                    )
                self._validate_plugin(plugin_root, state.active_version)
            plugin_info = plugin_root.lstat()
            token = self._token_factory()
            if _TOKEN.fullmatch(token) is None:
                raise LifecycleError("invalid_token", "Invalid uninstall token")
            if coordinator_token is not None and _TOKEN.fullmatch(coordinator_token) is None:
                raise LifecycleError("invalid_token", "Invalid coordinator token")
            if coordinator_product_id is not None and (
                not isinstance(coordinator_product_id, str)
                or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}", coordinator_product_id)
                is None
            ):
                raise LifecycleError("invalid_product_id", "Invalid coordinator product identity")
            pending_value: dict[str, object] = {
                "schema": STATE_SCHEMA,
                "token": token,
                "generation": generation,
                "plugin_root": str(plugin_root),
                "plugin_device": plugin_info.st_dev,
                "plugin_inode": plugin_info.st_ino,
                "created_at": self._now(),
            }
            if coordinator_token is not None:
                pending_value["coordinator_token"] = coordinator_token
            if coordinator_product_id is not None:
                pending_value["coordinator_product_id"] = coordinator_product_id
            self._write_json(
                self.paths.pending_uninstall_file,
                pending_value,
            )
        self._checked_systemctl_quick(
            "--no-block", "restart", "mango-overlay-cleanup.timer"
        )
        return token

    def finalize_pending_uninstall(self, token: str | None = None) -> bool:
        # Read and validate the pending record under the lifecycle lock, then
        # release it before taking the coordinator lock. Registration takes
        # the locks in the opposite order (coordinator -> lifecycle), so this
        # two-phase shape avoids a cross-product deadlock.
        with self._locked():
            pending = self._read_pending()
            if pending is None:
                self._stop_cleanup_timer()
                return False
            if token is not None and pending["token"] != token:
                return False
            state = self._read_state()
            if state is None or state.generation != pending["generation"]:
                if not self._pending_is_coordinated(pending):
                    self._unlink_if_exists(self.paths.pending_uninstall_file)
                    self._stop_cleanup_timer()
                    return False
            plugin_presence = self._pending_plugin_presence(pending)
            if plugin_presence in {"original", "ambiguous"}:
                return False
            if plugin_presence == "replacement":
                self._unlink_if_exists(self.paths.pending_uninstall_file)
                self._stop_cleanup_timer()
                return False
            if self._now() - pending["created_at"] < UNINSTALL_GRACE_SECONDS:
                return False

        shared_result = self._finalize_coordinator_claim(pending)
        if shared_result == "wait":
            return False
        if shared_result == "retained":
            with self._locked():
                current = self._read_pending()
                if current is not None and (
                    token is None or current["token"] == token
                ):
                    self._unlink_if_exists(self.paths.pending_uninstall_file)
                    self._stop_cleanup_timer()
            return True

        with self._locked():
            # Re-check after the coordinator transaction; a replacement may
            # have arrived while the locks were released.
            current = self._read_pending()
            if current is None:
                self._stop_cleanup_timer()
                return False
            if token is not None and current["token"] != token:
                return False
            state = self._read_state()
            if (
                not self._pending_is_coordinated(current)
                and (state is None or state.generation != current["generation"])
            ):
                self._unlink_if_exists(self.paths.pending_uninstall_file)
                self._stop_cleanup_timer()
                return False
            presence = self._pending_plugin_presence(current)
            if presence in {"original", "ambiguous"}:
                return False
            if presence == "replacement":
                self._unlink_if_exists(self.paths.pending_uninstall_file)
                self._stop_cleanup_timer()
                return False

            cleanup_roots = (
                self.paths.data_root,
                self.paths.libexec_root,
                self.paths.config_root,
                self.paths.cache_root,
                self.paths.decky_settings_root,
                self.paths.decky_runtime_root,
                self.paths.decky_logs_root,
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
        core_version = manifest.get("core_version", version)
        if not isinstance(core_version, str):
            raise LifecycleError(
                "invalid_core_version", "Runtime manifest core version is invalid"
            )
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
        runtime_id = self._runtime_id(version, core_version, entries)

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
        core_version: str,
        entries: dict[str, dict[str, object]],
    ) -> str:
        canonical = {
            "schema": MANIFEST_SCHEMA,
            "version": version,
            "core_version": core_version,
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

    def _install_integration_files(self, plugin_root: Path | None) -> bool:
        self._ensure_private_directory(self.paths.libexec_root)
        source_root = (
            plugin_root / "py_modules/mango_overlay_decky"
            if plugin_root is not None
            else self.paths.libexec_root
        )
        changed = False
        for name in ("lifecycle.py", "launcher.py", "coordinator.py"):
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
                "[Timer]\nOnActiveSec=3s\nOnUnitActiveSec=5s\n"
                "AccuracySec=500ms\n"
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
            "coordinator": self.paths.libexec_root / "coordinator.py",
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
        if not isinstance(raw, dict) or not set(raw).issubset(paths):
            raise LifecycleError(
                "invalid_transaction", "Integration snapshot is invalid"
            )
        decoded: dict[Path, tuple[bytes, int] | None] = {}
        total_size = 0
        for name, path in paths.items():
            # ``coordinator.py`` was added after the first lifecycle schema;
            # an interrupted old transaction simply has no snapshot entry for
            # that helper and should recover as ``None``.
            entry = raw.get(name)
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
        self._checked_systemctl("enable", "mango-overlay-cleanup.timer")
        self._checked_systemctl(
            "enable",
            "--now",
            "mango-overlayd.socket",
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
        retain_candidate = transaction.get("retain_candidate", False)
        if not isinstance(retain_candidate, bool):
            raise LifecycleError("invalid_transaction", "Candidate retention flag is invalid")
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
        if (
            isinstance(candidate, str)
            and not retain_candidate
            and candidate not in retained
        ):
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

    def _validate_pending_plugin_root(self, plugin_root: Path) -> None:
        """Validate a coordinated product directory without assuming ownership."""

        try:
            info = plugin_root.lstat()
        except FileNotFoundError as error:
            raise LifecycleError("invalid_plugin", "Decky plugin root is missing") from error
        if not _plugin_directory_is_trusted(info, self._uid):
            raise LifecycleError("invalid_plugin", "Decky plugin root is unsafe")
        plugin_manifest = self._read_plugin_json(plugin_root / "plugin.json", 65536)
        package_manifest = self._read_plugin_json(plugin_root / "package.json", 65536)
        if plugin_manifest.get("name") != PLUGIN_NAME:
            raise LifecycleError("invalid_plugin", "Decky plugin identity differs")
        version = package_manifest.get("version")
        if not isinstance(version, str):
            raise LifecycleError("invalid_plugin", "Decky plugin version is invalid")
        self._validate_version(version)

    def _pending_plugin_presence(self, pending: dict[str, object]) -> str:
        plugin_root = Path(str(pending["plugin_root"]))
        try:
            plugin_info = plugin_root.lstat()
        except FileNotFoundError:
            pass
        except OSError as error:
            raise LifecycleError(
                "plugin_scan_failed", f"Could not inspect plugin root: {error}"
            ) from error
        else:
            device = pending.get("plugin_device")
            inode = pending.get("plugin_inode")
            if device is None or inode is None:
                return "ambiguous"
            if plugin_info.st_dev == device and plugin_info.st_ino == inode:
                return "original"
            if self._plugin_identity_matches(plugin_root):
                return "replacement"
            return "ambiguous"

        plugins_root = plugin_root.parent
        try:
            plugins_info = plugins_root.lstat()
        except FileNotFoundError:
            return "absent"
        except OSError as error:
            raise LifecycleError(
                "plugin_scan_failed", f"Could not inspect plugins root: {error}"
            ) from error
        if not _plugin_directory_is_trusted(plugins_info, self._uid):
            raise LifecycleError("unsafe_plugin_root", "Decky plugins root is unsafe")

        count = 0
        try:
            for candidate in plugins_root.iterdir():
                count += 1
                if count > MAX_PLUGIN_DIRECTORIES:
                    raise LifecycleError(
                        "too_many_plugins",
                        "Decky plugins root has too many entries",
                    )
                if self._plugin_identity_matches(candidate):
                    return "replacement"
        except OSError as error:
            raise LifecycleError(
                "plugin_scan_failed", f"Could not scan plugins root: {error}"
            ) from error
        return "absent"

    def _plugin_identity_matches(self, plugin_root: Path) -> bool:
        try:
            info = plugin_root.lstat()
            if not _plugin_directory_is_trusted(info, self._uid):
                return False
            plugin_manifest = self._read_plugin_json(
                plugin_root / "plugin.json", 65536
            )
        except (FileNotFoundError, OSError, LifecycleError):
            return False
        return plugin_manifest.get("name") == PLUGIN_NAME

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
        plugin_device = raw.get("plugin_device")
        plugin_inode = raw.get("plugin_inode")
        created_at = raw.get("created_at")
        coordinator_token = raw.get("coordinator_token")
        coordinator_product_id = raw.get("coordinator_product_id")
        if (
            raw.get("schema") != STATE_SCHEMA
            or not isinstance(token, str)
            or _TOKEN.fullmatch(token) is None
            or not isinstance(generation, str)
            or _GENERATION.fullmatch(generation) is None
            or not isinstance(plugin_root, str)
            or not Path(plugin_root).is_absolute()
            or (plugin_device is None) != (plugin_inode is None)
            or (
                plugin_device is not None
                and (
                    not isinstance(plugin_device, int)
                    or plugin_device < 0
                    or not isinstance(plugin_inode, int)
                    or plugin_inode < 0
                )
            )
            or not isinstance(created_at, (int, float))
            or (
                coordinator_token is not None
                and (
                    not isinstance(coordinator_token, str)
                    or _TOKEN.fullmatch(coordinator_token) is None
                )
            )
            or (
                coordinator_product_id is not None
                and (
                    not isinstance(coordinator_product_id, str)
                    or re.fullmatch(
                        r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}",
                        coordinator_product_id,
                    )
                    is None
                )
            )
        ):
            raise LifecycleError("invalid_pending", "Pending uninstall state is invalid")
        return raw

    @staticmethod
    def _pending_is_coordinated(pending: dict[str, object]) -> bool:
        return (
            isinstance(pending.get("coordinator_token"), str)
            and isinstance(pending.get("coordinator_product_id"), str)
        )

    def _finalize_coordinator_claim(
        self, pending: dict[str, object]
    ) -> str:
        """Remove one shared claim, returning ``last``, ``retained`` or ``wait``.

        A pending record without coordinator metadata is an older single
        plugin installation and continues through the legacy finalizer.
        """

        token = pending.get("coordinator_token")
        product_id = pending.get("coordinator_product_id")
        if not isinstance(token, str) or not isinstance(product_id, str):
            coordinator_state = self.paths.data_root / "coordinator" / "state.json"
            if not self._path_exists(coordinator_state):
                return "last"
            try:
                raw = self._read_json(coordinator_state, 512 * 1024)
                claims = raw.get("claims")
                return "wait" if isinstance(claims, list) and claims else "last"
            except LifecycleError:
                return "wait"
        try:
            try:
                from .coordinator import (
                    CoordinatorPaths,
                    RuntimeCoordinator,
                )
            except ImportError:
                from coordinator import (  # type: ignore[no-redef]
                    CoordinatorPaths,
                    RuntimeCoordinator,
                )
            operations = LifecycleRuntimeOperations(
                self,
                plugin_root=None,
                plugin_version=None,
                generation=str(pending["generation"]),
            )
            coordinator = RuntimeCoordinator(
                CoordinatorPaths.for_home(self.paths.home), operations
            )
            before = coordinator.status()
            if not any(claim.product_id == product_id for claim in before.claims):
                if not before.claims:
                    return "last"
                if before.active_revision is None:
                    return "wait"
                # Another finalizer may have removed this claim already.  The
                # remaining claims still own the shared runtime, so converge
                # the local lifecycle pointer and finish this stale pending
                # record instead of retrying it forever.
                self._sync_state_from_coordinator(before)
                return "retained"
            outcome = coordinator.finalize_removal(
                product_id,
                str(pending["generation"]),
                token=token,
                plugin_present=False,
            )
            if outcome.action in {"pending", "blocked", "failed"}:
                return "wait"
            after = coordinator.status()
            if not after.claims:
                return "last"
            if after.active_revision is None:
                return "wait"
            self._sync_state_from_coordinator(after)
            return "retained"
        except Exception:
            # The pending record remains for a later retry if coordinator state
            # is temporarily unavailable or malformed.
            return "wait"

    def _sync_state_from_coordinator(self, status: object) -> None:
        active_revision = getattr(status, "active_revision", None)
        claims = getattr(status, "claims", ())
        if not isinstance(active_revision, str):
            raise LifecycleError("coordinator_state_invalid", "No active shared revision")
        active_claim = next(
            (
                claim
                for claim in claims
                if getattr(claim.candidate, "content_revision", None)
                == active_revision
            ),
            None,
        )
        if active_claim is None:
            raise LifecycleError("coordinator_state_invalid", "Active claim is missing")
        runtime = self.ensure_prepared_runtime(active_revision)
        manifest = self._read_json(runtime / "manifest.json", 1024 * 1024)
        version = manifest.get("version")
        generation = getattr(active_claim, "generation", None)
        if not isinstance(version, str) or not isinstance(generation, str):
            raise LifecycleError("coordinator_state_invalid", "Active claim metadata is invalid")
        with self._locked():
            self._write_state(
                LifecycleState(
                    active_version=version,
                    previous_version=None,
                    generation=generation,
                    active_runtime=active_revision,
                    previous_runtime=None,
                )
            )

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

    def _checked_systemctl_quick(self, *arguments: str) -> None:
        command = ["systemctl", "--user", *arguments]
        try:
            result = self._run_quick_command(command)
        except (OSError, subprocess.SubprocessError) as error:
            raise LifecycleError(
                "systemctl_failed", f"{' '.join(command)} failed: {error}"
            ) from error
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise LifecycleError(
                "systemctl_failed",
                f"{' '.join(command)} failed: {detail}",
            )

    def _stop_cleanup_timer(self) -> None:
        try:
            self._run_command(
                ["systemctl", "--user", "stop", "mango-overlay-cleanup.timer"]
            )
        except (OSError, subprocess.SubprocessError):
            pass

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
