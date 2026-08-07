#!/usr/bin/python3
from __future__ import annotations

import argparse
import json
import os
import re
import signal
import stat
import subprocess
import sys
from pathlib import Path

try:
    from .coordinator import (
        CoordinatorError,
        CoordinatorPaths,
        RuntimeCandidate,
        RuntimeClaim,
        RuntimeCoordinator,
    )
    from .lifecycle import LifecycleManager, LifecyclePaths, LifecycleRuntimeOperations
except ImportError:  # this helper is installed as a flat libexec script
    from coordinator import (  # type: ignore[no-redef]
        CoordinatorError,
        CoordinatorPaths,
        RuntimeCandidate,
        RuntimeClaim,
        RuntimeCoordinator,
    )
    from lifecycle import (  # type: ignore[no-redef]
        LifecycleManager,
        LifecyclePaths,
        LifecycleRuntimeOperations,
    )


VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}")
SYSTEM_MANGOAPP = Path("/usr/bin/mangoapp")
DESKTOP_RUNTIME_FILES = (
    "lib/libMangoHud.so",
    "lib/libMangoHud_opengl.so",
    "lib/libMangoHud_shim.so",
    "lib32/libMangoHud.so",
    "lib32/libMangoHud_opengl.so",
    "lib32/libMangoHud_shim.so",
    "share/vulkan/implicit_layer.d/MangoOverlay.x86_64.json",
    "share/vulkan/implicit_layer.d/MangoOverlay.x86.json",
)

COORDINATOR_REVISION = re.compile(r"[a-f0-9]{64}")
MAX_COORDINATOR_JSON = 512 * 1024


def active_runtime() -> Path | None:
    home = Path.home()
    state_path = home / ".local/state/mango-overlay-decky/install.json"
    coordinator_path = home / ".local/share/mango-overlay-decky/coordinator/state.json"
    try:
        coordinator_exists = False
        try:
            coordinator_info = coordinator_path.lstat()
            coordinator_exists = True
        except FileNotFoundError:
            coordinator_info = None
        if coordinator_exists:
            assert coordinator_info is not None
            if (
                not stat.S_ISREG(coordinator_info.st_mode)
                or coordinator_info.st_uid != os.geteuid()
                or coordinator_info.st_mode & 0o022
                or coordinator_info.st_size > 512 * 1024
            ):
                return None
            coordinator_state = json.loads(
                coordinator_path.read_text(encoding="utf-8")
            )
            if not isinstance(coordinator_state, dict) or coordinator_state.get("schema") != 1:
                return None
            coordinator_revision = coordinator_state.get("active_revision")
            if coordinator_revision is None:
                return None
            if COORDINATOR_REVISION.fullmatch(coordinator_revision) is None:
                return None
            runtime_id = coordinator_revision
            runtime = (
                home
                / ".local/share/mango-overlay-decky/runtime/versions"
                / runtime_id
            )
            runtime_info = runtime.lstat()
            if (
                not stat.S_ISDIR(runtime_info.st_mode)
                or runtime_info.st_uid != os.geteuid()
                or runtime_info.st_mode & 0o022
            ):
                return None
            return runtime
        else:
            runtime_id = None
        info = state_path.lstat()
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_mode & 0o022
            or info.st_size > 64 * 1024
        ):
            return None
        state = json.loads(state_path.read_text(encoding="utf-8"))
        version = state.get("active_version")
        if runtime_id is None:
            runtime_id = state.get("active_runtime", version)
        if (
            state.get("schema") != 1
            or VERSION.fullmatch(version or "") is None
            or VERSION.fullmatch(runtime_id or "") is None
        ):
            return None
        runtime = (
            home
            / ".local/share/mango-overlay-decky/runtime/versions"
            / runtime_id
        )
        runtime_info = runtime.lstat()
        if (
            not stat.S_ISDIR(runtime_info.st_mode)
            or runtime_info.st_uid != os.geteuid()
            or runtime_info.st_mode & 0o022
        ):
            return None
        return runtime
    except (OSError, UnicodeError, ValueError, TypeError):
        return None


def _runtime_file(
    runtime: Path, relative: str, *, executable: bool = False
) -> Path | None:
    path = runtime / relative
    try:
        info = path.lstat()
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_mode & 0o022
            or (executable and not os.access(path, os.X_OK))
        ):
            return None
        return path
    except OSError:
        return None


def active_binary(role: str) -> Path | None:
    runtime = active_runtime()
    if runtime is None:
        return None
    try:
        names = {
            "mangoapp": "mangoapp",
            "broker": "mango-overlayd",
            "controller": "mango-overlayctl",
            "test-provider": "mango-overlay-test-provider",
        }
        name = names.get(role)
        if name is None:
            return None
        return _runtime_file(runtime, f"bin/{name}", executable=True)
    except (ValueError, TypeError):
        return None


def desktop_environment(
    runtime: Path, inherited: dict[str, str]
) -> dict[str, str] | None:
    if any(
        _runtime_file(runtime, relative) is None
        for relative in DESKTOP_RUNTIME_FILES
    ):
        return None

    environment = inherited.copy()
    library_directory = f"{runtime}/$LIB"
    shim = f"{library_directory}/libMangoHud_shim.so"
    opengl = f"{library_directory}/libMangoHud_opengl.so"
    manifests = runtime / "share/vulkan/implicit_layer.d"
    inherited_preloads = [
        entry
        for entry in re.split(r"[:\s]+", environment.get("LD_PRELOAD", ""))
        if entry and Path(entry).name != "libMangoHud_shim.so"
    ]

    for name in (
        "DISABLE_MANGOHUD",
        "VK_INSTANCE_LAYERS",
        "VK_LOADER_LAYERS_DISABLE",
        "VK_LOADER_LAYERS_ENABLE",
    ):
        environment.pop(name, None)
    environment["MANGOHUD"] = "1"
    environment["MANGOHUD_OPENGL_LIBS"] = opengl
    environment["MANGO_OVERLAY_SOCKET"] = (
        f"/run/user/{os.geteuid()}/mango-overlay-decky.sock"
    )
    environment["VK_IMPLICIT_LAYER_PATH"] = str(manifests)
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        part
        for part in (library_directory, environment.get("LD_LIBRARY_PATH", ""))
        if part
    )
    environment["LD_PRELOAD"] = os.pathsep.join((shim, *inherited_preloads))
    return environment


def run_managed_mangoapp(
    binary: Path,
    arguments: list[str],
    environment: dict[str, str],
    fallback_environment: dict[str, str],
) -> int:
    child: subprocess.Popen[bytes] | None = None
    stopping = False

    def request_stop(number: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True
        if child is not None and child.poll() is None:
            try:
                child.send_signal(number)
            except ProcessLookupError:
                pass

    previous_handlers = {
        number: signal.signal(number, request_stop)
        for number in (signal.SIGTERM, signal.SIGINT)
    }
    status: int | None = None
    try:
        child = subprocess.Popen(
            arguments,
            executable=binary,
            env=environment,
        )
        if stopping and child.poll() is None:
            child.terminate()
        status = child.wait()
    except OSError as error:
        print(f"Managed MangoApp could not start: {error}", file=sys.stderr)
    finally:
        for number, handler in previous_handlers.items():
            signal.signal(number, handler)

    if stopping:
        return 0
    print(
        f"Managed MangoApp exited with status {status}; using the system MangoApp",
        file=sys.stderr,
    )
    os.execve(SYSTEM_MANGOAPP, [SYSTEM_MANGOAPP.name], fallback_environment)
    return 70


def _candidate_json(candidate: RuntimeCandidate) -> dict[str, object]:
    return {
        "core_version": candidate.core_version.as_string(),
        "content_revision": candidate.content_revision,
        "runtime_ref": candidate.runtime_ref,
        "coordinator_schema": candidate.coordinator_schema,
        "provider_protocol_min": candidate.provider_protocol_min,
        "provider_protocol_max": candidate.provider_protocol_max,
    }


def _coordinator_status_json(coordinator: RuntimeCoordinator) -> dict[str, object]:
    status = coordinator.status()
    return {
        "active_revision": status.active_revision,
        "known_good_revision": status.known_good_revision,
        "failed_revisions": list(status.failed_revisions),
        "verified_revisions": list(getattr(status, "verified_revisions", ())),
        "last_error": status.last_error,
        "claims": [
            {
                "product_id": claim.product_id,
                "generation": claim.generation,
                "pending_removal": claim.pending_removal,
                "pending_token": claim.pending_token,
                "candidate": _candidate_json(claim.candidate),
            }
            for claim in status.claims
        ],
    }


def _print_json(value: object) -> int:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
    if len(encoded.encode("utf-8")) > MAX_COORDINATOR_JSON:
        return 70
    print(encoded)
    return 0


def _coordinator_instance() -> tuple[RuntimeCoordinator, LifecycleManager]:
    home = Path.home()
    lifecycle = LifecycleManager(LifecyclePaths.for_home(home))
    operations = LifecycleRuntimeOperations(
        lifecycle,
        plugin_root=None,
        plugin_version=None,
        generation="launcher-00000000",
    )
    return RuntimeCoordinator(CoordinatorPaths.for_home(home), operations), lifecycle


def _coordinator_candidate(arguments: object) -> RuntimeCandidate:
    # argparse.Namespace is intentionally treated as an opaque object here so
    # this parser remains easy to exercise without a running Decky process.
    core_version = getattr(arguments, "core_version")
    revision = getattr(arguments, "content_revision")
    runtime_ref = getattr(arguments, "runtime_ref")
    return RuntimeCandidate.create(
        core_version=core_version,
        content_revision=revision,
        runtime_ref=runtime_ref,
        provider_protocol_min=getattr(arguments, "provider_protocol_min", 1),
        provider_protocol_max=getattr(arguments, "provider_protocol_max", 1),
    )


def coordinator_main(arguments: object) -> int:
    """Run the small JSON coordinator command surface used by helpers/CI."""

    try:
        coordinator, _lifecycle = _coordinator_instance()
        command = getattr(arguments, "coordinator_command")
        if command == "status":
            return _print_json(_coordinator_status_json(coordinator))
        if command == "register":
            claim = RuntimeClaim(
                getattr(arguments, "product_id"),
                getattr(arguments, "generation"),
                _coordinator_candidate(arguments),
            )
            outcome = coordinator.register(claim)
            return _print_json(
                {
                    "action": outcome.action,
                    "active_revision": outcome.active_revision,
                    "candidate": (
                        _candidate_json(outcome.candidate)
                        if outcome.candidate is not None
                        else None
                    ),
                    "error": outcome.error,
                }
            )
        if command == "pending-remove":
            token = coordinator.mark_pending_removal(
                getattr(arguments, "product_id"),
                getattr(arguments, "generation"),
            )
            return _print_json({"token": token})
        if command == "finalize":
            outcome = coordinator.finalize_removal(
                getattr(arguments, "product_id"),
                getattr(arguments, "generation"),
                token=getattr(arguments, "token", None),
                plugin_present=bool(getattr(arguments, "plugin_present")),
            )
            return _print_json(
                {
                    "action": outcome.action,
                    "active_revision": outcome.active_revision,
                    "error": outcome.error,
                }
            )
        if command == "retry":
            outcome = coordinator.retry(getattr(arguments, "content_revision", None))
            return _print_json(
                {
                    "action": outcome.action,
                    "active_revision": outcome.active_revision,
                    "error": outcome.error,
                }
            )
        return 64
    except (CoordinatorError, OSError, ValueError) as error:
        print(f"coordinator error: {error}", file=sys.stderr)
        return 65


def coordinator_cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="mango-overlay-launcher coordinator")
    subcommands = parser.add_subparsers(dest="coordinator_command", required=True)
    subcommands.add_parser("status")

    register = subcommands.add_parser("register")
    register.add_argument("--product-id", required=True)
    register.add_argument("--generation", required=True)
    register.add_argument("--core-version", required=True)
    register.add_argument("--content-revision", required=True)
    register.add_argument("--runtime-ref", required=True)
    register.add_argument("--provider-protocol-min", type=int, default=1)
    register.add_argument("--provider-protocol-max", type=int, default=1)

    pending = subcommands.add_parser("pending-remove")
    pending.add_argument("--product-id", required=True)
    pending.add_argument("--generation", required=True)

    finalize = subcommands.add_parser("finalize")
    finalize.add_argument("--product-id", required=True)
    finalize.add_argument("--generation", required=True)
    finalize.add_argument("--token")
    presence = finalize.add_mutually_exclusive_group(required=True)
    presence.add_argument("--plugin-present", action="store_true")
    presence.add_argument("--plugin-absent", action="store_false", dest="plugin_present")

    retry = subcommands.add_parser("retry")
    retry.add_argument("content_revision", nargs="?")
    arguments = parser.parse_args(argv)
    return coordinator_main(arguments)


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "coordinator":
        return coordinator_cli(sys.argv[2:])
    if len(sys.argv) < 2 or sys.argv[1] not in {
        "mangoapp",
        "broker",
        "controller",
        "test-provider",
        "desktop",
    }:
        return 64
    role = sys.argv[1]
    if role == "desktop":
        if len(sys.argv) < 4 or sys.argv[2] != "--":
            return 64
        command = sys.argv[3:]
        inherited_environment = os.environ.copy()
        runtime = active_runtime()
        environment = (
            desktop_environment(runtime, inherited_environment)
            if runtime is not None
            else None
        )
        if environment is None:
            environment = inherited_environment
        try:
            os.execvpe(command[0], command, environment)
        except OSError as error:
            print(f"Game could not start: {error}", file=sys.stderr)
            return 70
        return 70

    home = Path.home()
    binary = active_binary(role)
    managed_runtime = binary is not None
    if binary is None:
        if role == "mangoapp":
            binary = SYSTEM_MANGOAPP
        else:
            return 69
    arguments = [binary.name]
    if role == "broker":
        arguments += [
            "--policy-file",
            str(home / ".config/mango-overlay-decky/policy.fb"),
        ]
    elif role == "controller":
        arguments += sys.argv[2:]
    fallback_environment = os.environ.copy()
    environment = fallback_environment.copy()
    if managed_runtime:
        library_path = str(binary.parent.parent / "lib")
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(
            part
            for part in (library_path, environment.get("LD_LIBRARY_PATH", ""))
            if part
        )
    if role == "mangoapp" and managed_runtime:
        return run_managed_mangoapp(
            binary,
            arguments,
            environment,
            fallback_environment,
        )
    os.execve(binary, arguments, environment)
    return 70


if __name__ == "__main__":
    raise SystemExit(main())
