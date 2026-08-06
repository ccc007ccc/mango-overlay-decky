#!/usr/bin/python3
from __future__ import annotations

import json
import os
import re
import signal
import stat
import subprocess
import sys
from pathlib import Path


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


def active_runtime() -> Path | None:
    home = Path.home()
    state_path = home / ".local/state/mango-overlay-decky/install.json"
    try:
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


def main() -> int:
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
