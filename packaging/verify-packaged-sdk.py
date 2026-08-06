#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from types import SimpleNamespace


PLUGIN_DIRECTORY = "mango-overlay-decky"


def extract_archive(archive_path: Path, destination: Path) -> Path:
    with zipfile.ZipFile(archive_path) as archive:
        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            file_type = (member.external_attr >> 16) & 0o170000
            if (
                path.is_absolute()
                or not path.parts
                or path.parts[0] != PLUGIN_DIRECTORY
                or any(part in ("", ".", "..") for part in path.parts)
                or file_type != stat.S_IFREG
            ):
                raise RuntimeError(f"unsafe package member: {member.filename}")
            target = destination.joinpath(*path.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            target.chmod((member.external_attr >> 16) & 0o777)
    return destination / PLUGIN_DIRECTORY


def environment_for(directory: Path, library_directory: Path) -> dict[str, str]:
    environment = os.environ.copy()
    home = directory / "home"
    runtime = directory / "runtime"
    for path in (home, runtime):
        path.mkdir(parents=True, mode=0o700)
    environment.update(
        {
            "HOME": str(home),
            "XDG_CACHE_HOME": str(home / ".cache"),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_STATE_HOME": str(home / ".local/state"),
            "LD_LIBRARY_PATH": os.pathsep.join(
                part
                for part in (
                    str(library_directory),
                    environment.get("LD_LIBRARY_PATH", ""),
                )
                if part
            ),
        }
    )
    return environment


def verify_lifecycle(plugin_root: Path, home: Path, version: str) -> Path:
    sys.path.insert(0, str(plugin_root / "py_modules"))
    try:
        from mango_overlay_decky.lifecycle import LifecycleManager, LifecyclePaths

        def fake_systemctl(command: list[str]) -> SimpleNamespace:
            return SimpleNamespace(
                returncode=(3 if "is-active" in command else 0),
                stdout="",
                stderr="",
            )

        manager = LifecycleManager(
            LifecyclePaths.for_home(home),
            run_command=fake_systemctl,
        )
        state = manager.activate(plugin_root, version, "a" * 32)
        if state.active_version != version:
            raise RuntimeError("temporary lifecycle activation selected the wrong version")
        if state.active_runtime is None:
            raise RuntimeError("temporary lifecycle activation has no runtime revision")
        analyzer = shutil.which("systemd-analyze")
        if analyzer is None:
            raise RuntimeError("systemd-analyze is required for package acceptance")
        environment = os.environ.copy()
        environment["HOME"] = str(home)
        subprocess.run(
            [analyzer, "--user", "verify", *(str(path) for path in manager._unit_paths())],
            check=True,
            env=environment,
            timeout=30,
        )
        return manager.paths.versions / state.active_runtime
    finally:
        sys.path.pop(0)


def run(command: list[str], environment: dict[str, str]) -> None:
    subprocess.run(command, check=True, env=environment, timeout=180)


def elf_identity(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:20]
    if len(header) != 20 or header[:4] != b"\x7fELF":
        raise RuntimeError(f"desktop runtime file is not ELF: {path}")
    elf_class = header[4]
    encoding = header[5]
    if elf_class not in (1, 2) or encoding not in (1, 2):
        raise RuntimeError(f"desktop runtime has an invalid ELF header: {path}")
    byteorder = "little" if encoding == 1 else "big"
    machine = int.from_bytes(header[18:20], byteorder=byteorder)
    return elf_class, machine


def verify_desktop_runtime(runtime: Path) -> None:
    architectures = (
        (
            runtime / "lib",
            2,
            62,
            (
                "libMangoHud.so",
                "libMangoHud_opengl.so",
                "libMangoHud_shim.so",
                "libgif.so.7",
            ),
        ),
        (
            runtime / "lib32",
            1,
            3,
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
    for directory, expected_class, expected_machine, names in architectures:
        for name in names:
            path = directory / name
            if elf_identity(path) != (expected_class, expected_machine):
                raise RuntimeError(f"desktop runtime has the wrong ABI: {path}")

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
    for path, expected_name, expected_library, target in manifests:
        manifest = json.loads(path.read_text(encoding="utf-8"))
        layer = manifest.get("layer")
        if not isinstance(layer, dict):
            raise RuntimeError(f"desktop Vulkan manifest is invalid: {path}")
        if (
            layer.get("name") != expected_name
            or layer.get("library_path") != expected_library
            or (path.parent / expected_library).resolve() != target.resolve()
        ):
            raise RuntimeError(f"desktop Vulkan manifest targets the wrong library: {path}")

    for relative in (
        "licenses/libgif7/copyright",
        "licenses/libsharpyuv0/copyright",
        "licenses/libwebp7/copyright",
    ):
        path = runtime / relative
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"desktop runtime license is missing: {path}")


def verify_desktop_launcher(home: Path, runtime: Path) -> None:
    launcher = home / ".local/libexec/mango-overlay-decky/launcher.py"
    environment = {
        "HOME": str(home),
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
    }
    result = subprocess.run(
        [str(launcher), "desktop", "--", "/usr/bin/env"],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
        env=environment,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"packaged desktop launcher returned {result.returncode}: {result.stderr}"
        )
    if "cannot be preloaded" in result.stderr:
        raise RuntimeError(f"packaged desktop shim was not loadable: {result.stderr}")
    launched = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    library_directory = f"{runtime}/$LIB"
    expected = {
        "MANGOHUD": "1",
        "MANGOHUD_OPENGL_LIBS": (
            f"{library_directory}/libMangoHud_opengl.so"
        ),
        "MANGO_OVERLAY_SOCKET": (
            f"/run/user/{os.geteuid()}/mango-overlay-decky.sock"
        ),
        "VK_IMPLICIT_LAYER_PATH": str(
            runtime / "share/vulkan/implicit_layer.d"
        ),
        "LD_LIBRARY_PATH": library_directory,
        "LD_PRELOAD": f"{library_directory}/libMangoHud_shim.so",
    }
    for name, value in expected.items():
        if launched.get(name) != value:
            raise RuntimeError(f"packaged desktop launcher set the wrong {name}")
    for name in ("MANGOHUD_CONFIGFILE", "MANGOHUD_CONFIG"):
        if name in launched:
            raise RuntimeError(f"packaged desktop launcher unexpectedly set {name}")


def verify_game_mode_only_runtime(home: Path, runtime: Path) -> None:
    forbidden = (
        runtime / "lib/libMangoHud.so",
        runtime / "lib/libMangoHud_opengl.so",
        runtime / "lib/libMangoHud_shim.so",
        runtime / "lib32",
        runtime / "share/vulkan/implicit_layer.d",
    )
    for path in forbidden:
        if path.exists():
            raise RuntimeError(
                f"game-mode package contains a desktop injection artifact: {path}"
            )

    launcher = home / ".local/libexec/mango-overlay-decky/launcher.py"
    environment = {
        "HOME": str(home),
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "MANGOHUD": "1",
        "MANGOHUD_CONFIG": "fps_only",
        "MANGO_OVERLAY_PACKAGE_TEST": "pass-through",
    }
    result = subprocess.run(
        [str(launcher), "desktop", "--", "/usr/bin/env"],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
        env=environment,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"disabled desktop launcher did not pass through: {result.stderr}"
        )
    launched = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    for name, value in environment.items():
        if launched.get(name) != value:
            raise RuntimeError(f"disabled desktop launcher changed {name}")
    for name in (
        "MANGO_OVERLAY_SOCKET",
        "MANGOHUD_OPENGL_LIBS",
        "VK_IMPLICIT_LAYER_PATH",
        "LD_PRELOAD",
        "LD_LIBRARY_PATH",
    ):
        if name in launched:
            raise RuntimeError(f"disabled desktop launcher injected {name}")


def verify_sdk(
    source_root: Path,
    development_build: Path,
    plugin_root: Path,
    temporary_root: Path,
) -> None:
    runtime = plugin_root / "runtime"
    library = runtime / "lib/libmango-overlay-client.so.1"
    broker = runtime / "bin/mango-overlayd"
    environment = environment_for(temporary_root / "sdk", library.parent)

    for name in (
        "mangoapp",
        "mango-overlayd",
        "mango-overlayctl",
        "mango-overlay-test-provider",
    ):
        run(
            [str(runtime / "bin" / name), "--mango-overlay-self-test"],
            environment,
        )

    run(
        [
            str(development_build / "client/mango-overlay-provider-client-process-test"),
            str(broker),
        ],
        environment,
    )
    run(
        [
            sys.executable,
            str(source_root / "client/tests/cpp_client_process_test.py"),
            str(broker),
            str(development_build / "client/mango-overlay-cpp-client-process-test"),
            str(library),
        ],
        environment,
    )
    run(
        [
            sys.executable,
            str(source_root / "client/tests/python_client_process_test.py"),
            str(broker),
            str(library),
            str(source_root / "client/python"),
        ],
        environment,
    )
    cargo = shutil.which("cargo", path=environment.get("PATH"))
    if cargo is None:
        raise RuntimeError("cargo is required to verify the Rust SDK")
    linker_directory = temporary_root / "sdk-link"
    linker_directory.mkdir()
    linker_library = linker_directory / "libmango-overlay-client.so"
    linker_library.symlink_to(library)
    run(
        [
            sys.executable,
            str(source_root / "client/tests/rust_client_process_test.py"),
            cargo,
            str(broker),
            str(linker_library),
            str(source_root / "client/rust/Cargo.toml"),
            str(temporary_root / "cargo-target"),
        ],
        environment,
    )


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify a packaged runtime and each provider SDK"
    )
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--development-build", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    options = arguments()
    with tempfile.TemporaryDirectory(prefix="mango-overlay-package-acceptance-") as raw:
        temporary = Path(raw)
        plugin_root = extract_archive(options.archive, temporary / "unpacked")
        package = json.loads(
            (plugin_root / "package.json").read_text(encoding="utf-8")
        )
        version = package.get("version")
        if not isinstance(version, str):
            raise RuntimeError("package.json version is invalid")
        lifecycle_home = temporary / "lifecycle-home"
        installed_runtime = verify_lifecycle(plugin_root, lifecycle_home, version)
        verify_game_mode_only_runtime(lifecycle_home, installed_runtime)
        verify_sdk(
            options.source_root.resolve(),
            options.development_build.resolve(),
            plugin_root,
            temporary,
        )
    print("packaged Game Mode lifecycle and C/C++/Python/Rust SDK tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
