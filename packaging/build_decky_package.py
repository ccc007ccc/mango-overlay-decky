#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import tempfile
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence


PLUGIN_DIRECTORY = "mango-overlay-decky"
PLUGIN_NAME = "Mango Overlay Decky"
DEFAULT_SOURCE_DATE_EPOCH = 315532800


class PackageError(RuntimeError):
    pass


@dataclass(frozen=True)
class PackageFile:
    archive_path: str
    source_path: Path | None
    mode: int
    contents: bytes | None = None


PLUGIN_FILES = (
    ("LICENSE", "LICENSE", 0o644),
    ("README.md", "README.md", 0o644),
    ("plugin.json", "plugin/plugin.json", 0o644),
    ("package.json", "plugin/package.json", 0o644),
    ("main.py", "plugin/main.py", 0o644),
    ("dist/index.js", "plugin/dist/index.js", 0o644),
    (
        "py_modules/mango_overlay_decky/__init__.py",
        "plugin/py_modules/mango_overlay_decky/__init__.py",
        0o644,
    ),
    (
        "py_modules/mango_overlay_decky/launcher.py",
        "plugin/py_modules/mango_overlay_decky/launcher.py",
        0o644,
    ),
    (
        "py_modules/mango_overlay_decky/lifecycle.py",
        "plugin/py_modules/mango_overlay_decky/lifecycle.py",
        0o644,
    ),
)

RUNTIME_FILES = (
    ("runtime/bin/mangoapp", "src/mangoapp", 0o755),
    ("runtime/bin/mango-overlayd", "broker/mango-overlayd", 0o755),
    ("runtime/bin/mango-overlayctl", "broker/mango-overlayctl", 0o755),
    (
        "runtime/bin/mango-overlay-test-provider",
        "tools/mango-overlay-test-provider",
        0o755,
    ),
    (
        "runtime/lib/libmango-overlay-client.so.1",
        "client/libmango-overlay-client.so.1.0.0",
        0o644,
    ),
    (
        "runtime/lib/libjpeg.so.62",
        "runtime-deps/libjpeg.so.62",
        0o644,
    ),
    (
        "runtime/licenses/libjpeg62-turbo/copyright",
        "runtime-deps/libjpeg62-turbo-copyright",
        0o644,
    ),
)

# Desktop renderer sources and development tools remain in the repository, but
# the first-release Decky package is intentionally Game Mode only.
VULKAN_LAYER_MANIFESTS = (
)


def _read_regular_file(path: Path) -> bytes:
    try:
        metadata = path.lstat()
    except FileNotFoundError as error:
        raise PackageError(f"Required package input is missing: {path}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise PackageError(f"Package input must be a regular file: {path}")
    return path.read_bytes()


def _read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(_read_regular_file(path))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PackageError(f"Invalid JSON file: {path}") from error
    if not isinstance(value, dict):
        raise PackageError(f"JSON root must be an object: {path}")
    return value


def _package_version(source_root: Path) -> str:
    plugin = _read_json(source_root / "plugin/plugin.json")
    package = _read_json(source_root / "plugin/package.json")
    version = package.get("version")
    if plugin.get("name") != PLUGIN_NAME:
        raise PackageError("plugin.json contains the wrong plugin name")
    if not isinstance(version, str) or not version or len(version) > 64:
        raise PackageError("package.json contains an invalid version")
    if any(character not in "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._+-" for character in version):
        raise PackageError("package.json version is unsafe for an archive name")
    return version


def _runtime_manifest(
    version: str,
    runtime_files: Iterable[PackageFile],
) -> bytes:
    entries = []
    for file in sorted(runtime_files, key=lambda item: item.archive_path):
        if file.contents is None:
            raise PackageError(f"Runtime payload was not loaded: {file.archive_path}")
        relative = PurePosixPath(file.archive_path).relative_to("runtime").as_posix()
        entries.append(
            {
                "path": relative,
                "sha256": hashlib.sha256(file.contents).hexdigest(),
                "mode": file.mode,
            }
        )
    manifest = {"schema": 1, "version": version, "files": entries}
    return (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _vulkan_layer_manifest(layer_name: str, library_path: str) -> bytes:
    manifest = {
        "file_format_version": "1.0.0",
        "layer": {
            "name": layer_name,
            "type": "GLOBAL",
            "api_version": "1.3.0",
            "library_path": library_path,
            "implementation_version": "1",
            "description": "Mango Overlay Decky Vulkan overlay",
            "functions": {
                "vkGetInstanceProcAddr": "overlay_GetInstanceProcAddr",
                "vkGetDeviceProcAddr": "overlay_GetDeviceProcAddr",
            },
            "enable_environment": {"MANGOHUD": "1"},
            "disable_environment": {"DISABLE_MANGOHUD": "1"},
        },
    }
    return (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()


def collect_package_files(source_root: Path, build_root: Path) -> tuple[str, list[PackageFile]]:
    source_root = source_root.resolve()
    build_root = build_root.resolve()
    version = _package_version(source_root)
    files: list[PackageFile] = []
    for archive_path, source_path, mode in PLUGIN_FILES:
        path = source_root / source_path
        files.append(PackageFile(archive_path, path, mode, _read_regular_file(path)))

    runtime_files: list[PackageFile] = []
    for archive_path, source_path, mode in RUNTIME_FILES:
        path = build_root / source_path
        runtime_files.append(
            PackageFile(archive_path, path, mode, _read_regular_file(path))
        )
    for archive_path, layer_name, library_path in VULKAN_LAYER_MANIFESTS:
        runtime_files.append(
            PackageFile(
                archive_path,
                None,
                0o644,
                _vulkan_layer_manifest(layer_name, library_path),
            )
        )
    files.extend(runtime_files)
    files.append(
        PackageFile(
            "runtime/manifest.json",
            None,
            0o644,
            _runtime_manifest(version, runtime_files),
        )
    )

    archive_paths = [file.archive_path for file in files]
    if len(archive_paths) != len(set(archive_paths)):
        raise PackageError("Package file list contains duplicate paths")
    return version, sorted(files, key=lambda file: file.archive_path)


def _zip_timestamp(source_date_epoch: int) -> tuple[int, int, int, int, int, int]:
    minimum = DEFAULT_SOURCE_DATE_EPOCH
    maximum = 4354819199  # 2107-12-31 23:59:59 UTC, ZIP's upper bound.
    epoch = min(max(source_date_epoch, minimum), maximum)
    value = time.gmtime(epoch)
    return (value.tm_year, value.tm_mon, value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec)


def _write_archive(
    destination: Path,
    files: Sequence[PackageFile],
    source_date_epoch: int,
) -> None:
    timestamp = _zip_timestamp(source_date_epoch)
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(
            temporary,
            "w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
        ) as archive:
            for file in files:
                if file.contents is None:
                    raise PackageError(f"Package payload was not loaded: {file.archive_path}")
                info = zipfile.ZipInfo(
                    f"{PLUGIN_DIRECTORY}/{file.archive_path}",
                    date_time=timestamp,
                )
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (stat.S_IFREG | file.mode) << 16
                archive.writestr(info, file.contents, compresslevel=9)
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def verify_archive(path: Path, expected_files: Sequence[PackageFile]) -> None:
    expected = {
        f"{PLUGIN_DIRECTORY}/{file.archive_path}": file for file in expected_files
    }
    try:
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            actual = {member.filename: member for member in members}
            if len(actual) != len(members) or set(actual) != set(expected):
                raise PackageError("Decky archive file set differs from the package plan")
            for name, file in expected.items():
                member = actual[name]
                mode = (member.external_attr >> 16) & 0o7777
                kind = (member.external_attr >> 16) & stat.S_IFMT(0o170000)
                if kind != stat.S_IFREG or mode != file.mode:
                    raise PackageError(f"Decky archive mode differs: {name}")
                if archive.read(member) != file.contents:
                    raise PackageError(f"Decky archive contents differ: {name}")
            bad_member = archive.testzip()
            if bad_member is not None:
                raise PackageError(f"Decky archive CRC failed: {bad_member}")
    except (OSError, zipfile.BadZipFile) as error:
        raise PackageError(f"Invalid Decky archive: {path}") from error


def build_package(
    source_root: Path,
    build_root: Path,
    output_directory: Path,
    source_date_epoch: int,
) -> Path:
    version, files = collect_package_files(source_root, build_root)
    destination = output_directory / f"{PLUGIN_DIRECTORY}-{version}.zip"
    _write_archive(destination, files, source_date_epoch)
    verify_archive(destination, files)
    return destination


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the Decky distribution archive")
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument(
        "--source-date-epoch",
        type=int,
        default=int(os.environ.get("SOURCE_DATE_EPOCH", DEFAULT_SOURCE_DATE_EPOCH)),
    )
    return parser.parse_args()


def main() -> int:
    arguments = _arguments()
    try:
        archive = build_package(
            arguments.source_root,
            arguments.build_root,
            arguments.output_directory,
            arguments.source_date_epoch,
        )
    except PackageError as error:
        raise SystemExit(f"package error: {error}") from error
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
