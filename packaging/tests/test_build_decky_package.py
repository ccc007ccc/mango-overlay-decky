from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path

from build_decky_package import (
    PLUGIN_DIRECTORY,
    PLUGIN_FILES,
    RUNTIME_FILES,
    VULKAN_LAYER_MANIFESTS,
    PackageError,
    build_package,
)


class DeckyPackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="mango-overlay-package-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.output = self.root / "output"
        for _, relative, _ in PLUGIN_FILES:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if relative == "plugin/plugin.json":
                contents = json.dumps({"name": "Mango Overlay Decky"}).encode()
            elif relative == "plugin/package.json":
                contents = json.dumps(
                    {"name": "mango-overlay-decky", "version": "0.1.0"}
                ).encode()
            else:
                contents = f"source:{relative}\n".encode()
            path.write_bytes(contents)
        for _, relative, _ in RUNTIME_FILES:
            path = self.build / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"runtime:{relative}\n".encode())

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_archive_contains_only_audited_files_and_runtime_manifest(self) -> None:
        archive_path = build_package(
            self.source, self.build, self.output, source_date_epoch=1_700_000_000
        )

        self.assertEqual(archive_path.name, "mango-overlay-decky-0.1.0.zip")
        with zipfile.ZipFile(archive_path) as archive:
            names = archive.namelist()
            expected = sorted(
                [f"{PLUGIN_DIRECTORY}/{path}" for path, _, _ in PLUGIN_FILES]
                + [f"{PLUGIN_DIRECTORY}/{path}" for path, _, _ in RUNTIME_FILES]
                + [
                    f"{PLUGIN_DIRECTORY}/{path}"
                    for path, _, _ in VULKAN_LAYER_MANIFESTS
                ]
                + [f"{PLUGIN_DIRECTORY}/runtime/manifest.json"]
            )
            self.assertEqual(names, expected)
            manifest = json.loads(
                archive.read(f"{PLUGIN_DIRECTORY}/runtime/manifest.json")
            )
            self.assertEqual(manifest["schema"], 1)
            self.assertEqual(manifest["version"], "0.1.0")
            self.assertEqual(
                [entry["path"] for entry in manifest["files"]],
                sorted(
                    [path.removeprefix("runtime/") for path, _, _ in RUNTIME_FILES]
                    + [
                        path.removeprefix("runtime/")
                        for path, _, _ in VULKAN_LAYER_MANIFESTS
                    ]
                ),
            )
            for entry in manifest["files"]:
                contents = archive.read(
                    f"{PLUGIN_DIRECTORY}/runtime/{entry['path']}"
                )
                self.assertEqual(entry["sha256"], hashlib.sha256(contents).hexdigest())
            library = archive.getinfo(
                f"{PLUGIN_DIRECTORY}/runtime/lib/libmango-overlay-client.so.1"
            )
            self.assertEqual((library.external_attr >> 16) & 0o777, 0o644)

            forbidden_desktop_paths = (
                f"{PLUGIN_DIRECTORY}/runtime/lib32/",
                f"{PLUGIN_DIRECTORY}/runtime/lib/libMangoHud.so",
                f"{PLUGIN_DIRECTORY}/runtime/lib/libMangoHud_opengl.so",
                f"{PLUGIN_DIRECTORY}/runtime/lib/libMangoHud_shim.so",
                f"{PLUGIN_DIRECTORY}/runtime/share/vulkan/implicit_layer.d/",
            )
            for name in names:
                self.assertFalse(
                    name.startswith(forbidden_desktop_paths),
                    f"game-mode package unexpectedly contains desktop runtime: {name}",
                )

            for archive_path, layer_name, library_path in VULKAN_LAYER_MANIFESTS:
                layer = json.loads(
                    archive.read(f"{PLUGIN_DIRECTORY}/{archive_path}")
                )["layer"]
                self.assertEqual(layer["name"], layer_name)
                self.assertEqual(layer["library_path"], library_path)

    def test_archive_is_reproducible_and_ignores_input_mtime(self) -> None:
        first = build_package(
            self.source, self.build, self.output, source_date_epoch=1_700_000_000
        ).read_bytes()
        for path in (*self.source.rglob("*"), *self.build.rglob("*")):
            os.utime(path, (1_800_000_000, 1_800_000_000))
        second = build_package(
            self.source, self.build, self.output, source_date_epoch=1_700_000_000
        ).read_bytes()
        self.assertEqual(first, second)

    def test_runtime_manifest_keeps_core_version_separate_from_plugin_version(self) -> None:
        (self.source / "plugin/plugin.json").write_text(
            json.dumps(
                {
                    "name": "Mango Overlay Decky",
                    "mango_core_version": "3.2.1",
                }
            ),
            encoding="utf-8",
        )

        archive_path = build_package(
            self.source, self.build, self.output, source_date_epoch=1_700_000_000
        )

        with zipfile.ZipFile(archive_path) as archive:
            manifest = json.loads(
                archive.read(f"{PLUGIN_DIRECTORY}/runtime/manifest.json")
            )
        self.assertEqual(manifest["version"], "0.1.0")
        self.assertEqual(manifest["core_version"], "3.2.1")

    def test_missing_runtime_input_fails_without_an_archive(self) -> None:
        (self.build / RUNTIME_FILES[0][1]).unlink()
        with self.assertRaisesRegex(PackageError, "Required package input is missing"):
            build_package(
                self.source, self.build, self.output, source_date_epoch=1_700_000_000
            )
        self.assertFalse(self.output.exists())

    def test_symlinked_input_is_rejected(self) -> None:
        target = self.root / "outside"
        target.write_text("outside", encoding="utf-8")
        input_path = self.source / "README.md"
        input_path.unlink()
        input_path.symlink_to(target)

        with self.assertRaisesRegex(PackageError, "must be a regular file"):
            build_package(
                self.source, self.build, self.output, source_date_epoch=1_700_000_000
            )


if __name__ == "__main__":
    unittest.main()
