#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath


PLUGIN_DIRECTORY = "mango-overlay-decky"


def _user_environment() -> dict[str, str]:
    environment = os.environ.copy()
    runtime_directory = f"/run/user/{os.geteuid()}"
    environment["XDG_RUNTIME_DIR"] = runtime_directory
    environment["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={runtime_directory}/bus"
    return environment


def _gamescope_state(environment: dict[str, str]) -> str:
    result = subprocess.run(
        [
            "systemctl",
            "--user",
            "show",
            "gamescope-mangoapp.service",
            "--property=ActiveState",
            "--value",
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
        env=environment,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"cannot inspect gamescope-mangoapp.service: {detail}")
    return result.stdout.strip()


def _extract_archive(archive_path: Path, destination: Path) -> Path:
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


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate a Decky package against the real desktop user manager"
    )
    parser.add_argument("archive", type=Path)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="activate the package; without this flag the command is read-only",
    )
    return parser.parse_args()


def main() -> int:
    arguments = _arguments()
    archive = arguments.archive.resolve(strict=True)
    if os.geteuid() == 0:
        raise RuntimeError("run this command as the SteamOS desktop user, not root")

    environment = _user_environment()
    gamescope_before = _gamescope_state(environment)
    with tempfile.TemporaryDirectory(prefix="mango-overlay-desktop-activation-") as raw:
        plugin_root = _extract_archive(archive, Path(raw))
        package = json.loads(
            (plugin_root / "package.json").read_text(encoding="utf-8")
        )
        version = package.get("version")
        if not isinstance(version, str):
            raise RuntimeError("package.json version is invalid")

        if not arguments.apply:
            print(
                json.dumps(
                    {
                        "archive": str(archive),
                        "gamescope_mangoapp": gamescope_before,
                        "mode": "read-only preflight",
                        "version": version,
                    },
                    ensure_ascii=False,
                    sort_keys=True,
                )
            )
            return 0

        if gamescope_before != "inactive":
            raise RuntimeError(
                "desktop activation requires gamescope-mangoapp.service to be inactive"
            )

        sys.path.insert(0, str(plugin_root / "py_modules"))
        try:
            from mango_overlay_decky.lifecycle import LifecycleManager, LifecyclePaths

            manager = LifecycleManager(LifecyclePaths.for_home(Path.home()))
            state = manager.activate(
                plugin_root,
                version,
                f"desktop-{secrets.token_hex(16)}",
            )
        finally:
            sys.path.pop(0)

        launcher = Path.home() / ".local/libexec/mango-overlay-decky/launcher.py"
        controller = subprocess.run(
            [str(launcher), "controller", "status"],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )
        if controller.returncode != 0:
            detail = controller.stderr.strip() or controller.stdout.strip()
            raise RuntimeError(f"installed broker check failed: {detail}")
        broker = json.loads(controller.stdout)

    gamescope_after = _gamescope_state(environment)
    if gamescope_after != "inactive":
        raise RuntimeError("desktop activation unexpectedly started MangoApp")
    print(
        json.dumps(
            {
                "active_version": state.active_version,
                "broker": broker,
                "gamescope_mangoapp": gamescope_after,
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print(f"desktop activation failed: {error}", file=sys.stderr)
        print(
            'recovery: python3 ~/.local/libexec/mango-overlay-decky/lifecycle.py '
            'restore-system --home "$HOME"',
            file=sys.stderr,
        )
        raise SystemExit(1)
