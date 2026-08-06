from __future__ import annotations

import hashlib
import fcntl
import json
import os
import shutil
import stat
import tempfile
import time
import unittest
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from mango_overlay_decky.lifecycle import (
    LifecycleError,
    LifecycleManager,
    LifecyclePaths,
    MAX_PLUGIN_DIRECTORIES,
    UNINSTALL_GRACE_SECONDS,
    _default_run,
    _default_run_quick,
    _plugin_directory_is_trusted,
    _plugin_file_is_trusted,
)


@dataclass
class CommandResult:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


class FakeSystemctl:
    def __init__(self, lock_path: Path) -> None:
        self.commands: list[tuple[str, ...]] = []
        self.gamescope_active = False
        self.test_provider_active = False
        self.fail_mango_restarts = 0
        self.fail_integration_enables = 0
        self.crash_on_broker_restart = False
        self.cleanup_service_delay = 0.0
        self.cleanup_started_while_locked = False
        self.lock_path = lock_path

    def __call__(self, command: list[str]) -> CommandResult:
        call = tuple(command)
        self.commands.append(call)
        if call[-3:] == ("is-active", "--quiet", "gamescope-mangoapp.service"):
            return CommandResult(0 if self.gamescope_active else 3)
        if call[-3:] == (
            "is-active",
            "--quiet",
            "mango-overlay-test-provider.service",
        ):
            return CommandResult(0 if self.test_provider_active else 3)
        if call[-2:] == ("restart", "gamescope-mangoapp.service"):
            if self.fail_mango_restarts > 0:
                self.fail_mango_restarts -= 1
                return CommandResult(1, stderr="restart failed")
            return CommandResult()
        if len(call) >= 4 and call[2:4] == ("enable", "--now"):
            if self.fail_integration_enables > 0:
                self.fail_integration_enables -= 1
                return CommandResult(1, stderr="enable failed")
        if call[-2:] == ("try-restart", "mango-overlayd.service"):
            if self.crash_on_broker_restart:
                self.crash_on_broker_restart = False
                raise KeyboardInterrupt("simulated power loss")
        if call[-2:] == ("start", "mango-overlay-test-provider.service"):
            self.test_provider_active = True
        if call[-2:] == ("stop", "mango-overlay-test-provider.service"):
            self.test_provider_active = False
        if call[-2:] == ("restart", "mango-overlay-test-provider.service"):
            self.test_provider_active = True
        if call[-2:] == ("start", "mango-overlay-cleanup.service"):
            time.sleep(self.cleanup_service_delay)
            descriptor = os.open(self.lock_path, os.O_RDWR)
            try:
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    self.cleanup_started_while_locked = True
                else:
                    fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)
        return CommandResult()


def write_file(path: Path, contents: bytes, mode: int) -> dict[str, object]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)
    path.chmod(mode)
    return {
        "path": path.as_posix(),
        "sha256": hashlib.sha256(contents).hexdigest(),
        "mode": mode,
    }


def make_plugin(parent: Path, version: str, marker: str) -> Path:
    plugin = parent / f"plugin-{version}-{marker}"
    runtime = plugin / "runtime"
    files = []
    for relative in (
        "bin/mangoapp",
        "bin/mango-overlayd",
        "bin/mango-overlayctl",
        "bin/mango-overlay-test-provider",
    ):
        contents = f"#!/bin/sh\n# {marker}\nexit 0\n".encode()
        entry = write_file(runtime / relative, contents, 0o755)
        entry["path"] = relative
        files.append(entry)
    library = write_file(
        runtime / "lib/libmango-overlay-client.so.1",
        marker.encode(),
        0o644,
    )
    library["path"] = "lib/libmango-overlay-client.so.1"
    files.append(library)
    (runtime / "manifest.json").write_text(
        json.dumps({"schema": 1, "version": version, "files": files}),
        encoding="utf-8",
    )
    helper = plugin / "py_modules/mango_overlay_decky/lifecycle.py"
    launcher = plugin / "py_modules/mango_overlay_decky/launcher.py"
    write_file(helper, f"# lifecycle helper {marker}\n".encode(), 0o644)
    write_file(launcher, f"# launcher {marker}\n".encode(), 0o644)
    (plugin / "plugin.json").write_text(
        json.dumps({"name": "Mango Overlay Decky"}),
        encoding="utf-8",
    )
    (plugin / "package.json").write_text(
        json.dumps({"name": "mango-overlay-decky", "version": version}),
        encoding="utf-8",
    )
    return plugin


class LifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="mango-overlay-lifecycle-")
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir(mode=0o700)
        self.paths = LifecyclePaths.for_home(self.home)
        self.systemctl = FakeSystemctl(self.paths.lock_file)
        self.now = 1000.0
        self.verified: list[tuple[Path, str]] = []
        self.manager = LifecycleManager(
            self.paths,
            run_command=self.systemctl,
            run_quick_command=self.systemctl,
            verify_runtime=lambda path, version: self.verified.append((path, version)),
            now=lambda: self.now,
            token_factory=lambda: "a" * 32,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_default_runner_targets_the_current_user_bus_in_decky(self) -> None:
        decky_environment = {
            "HOME": "/home/deck",
            "XDG_RUNTIME_DIR": "/run/user/0",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/0/bus",
        }
        with (
            patch.dict(os.environ, decky_environment, clear=True),
            patch("mango_overlay_decky.lifecycle.os.geteuid", return_value=1000),
            patch("mango_overlay_decky.lifecycle.subprocess.run") as run,
        ):
            run.return_value = CommandResult()
            _default_run(["systemctl", "--user", "daemon-reload"])

        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["HOME"], "/home/deck")
        self.assertEqual(environment["XDG_RUNTIME_DIR"], "/run/user/1000")
        self.assertEqual(
            environment["DBUS_SESSION_BUS_ADDRESS"],
            "unix:path=/run/user/1000/bus",
        )

    def test_quick_runner_uses_a_one_second_timeout(self) -> None:
        with patch("mango_overlay_decky.lifecycle.subprocess.run") as run:
            run.return_value = CommandResult()
            _default_run_quick(
                [
                    "systemctl",
                    "--user",
                    "--no-block",
                    "restart",
                    "mango-overlay-cleanup.timer",
                ]
            )

        self.assertEqual(run.call_args.kwargs["timeout"], 1)

    def test_decky_root_owned_plugin_directory_is_trusted_but_not_writable(self) -> None:
        root_owned = SimpleNamespace(st_mode=stat.S_IFDIR | 0o755, st_uid=0)
        writable = SimpleNamespace(st_mode=stat.S_IFDIR | 0o775, st_uid=0)

        self.assertTrue(_plugin_directory_is_trusted(root_owned, os.geteuid()))
        self.assertFalse(_plugin_directory_is_trusted(writable, os.geteuid()))

        root_manifest = SimpleNamespace(
            st_mode=stat.S_IFREG | 0o755,
            st_uid=0,
            st_size=1024,
        )
        writable_manifest = SimpleNamespace(
            st_mode=stat.S_IFREG | 0o777,
            st_uid=0,
            st_size=1024,
        )
        self.assertTrue(
            _plugin_file_is_trusted(root_manifest, os.geteuid(), 65536)
        )
        self.assertFalse(
            _plugin_file_is_trusted(writable_manifest, os.geteuid(), 65536)
        )

    def test_initial_install_and_loader_reload_do_not_restart_mangoapp(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        state = self.manager.activate(plugin, "0.1.0", "generation-one")

        self.assertEqual(state.active_version, "0.1.0")
        self.assertIsNone(state.previous_version)
        self.assertEqual(state.generation, "generation-one")
        self.assertIsNotNone(state.active_runtime)
        self.assertTrue(
            (self.paths.versions / state.active_runtime / "bin/mangoapp").is_file()
        )
        self.assertTrue(self.paths.mangoapp_dropin.is_file())
        self.assertNotIn(
            ("systemctl", "--user", "restart", "gamescope-mangoapp.service"),
            self.systemctl.commands,
        )
        timer = (
            self.paths.systemd_user_root / "mango-overlay-cleanup.timer"
        ).read_text(encoding="utf-8")
        self.assertIn("OnActiveSec=3s", timer)
        self.assertIn("OnUnitActiveSec=5s", timer)
        self.assertNotIn("OnBootSec", timer)
        self.assertIn(
            (
                "systemctl",
                "--user",
                "enable",
                "mango-overlay-cleanup.timer",
            ),
            self.systemctl.commands,
        )
        self.assertNotIn(
            (
                "systemctl",
                "--user",
                "enable",
                "--now",
                "mango-overlayd.socket",
                "mango-overlay-cleanup.timer",
            ),
            self.systemctl.commands,
        )

        command_count = len(self.systemctl.commands)
        reloaded = self.manager.activate(plugin, "0.1.0", "generation-two")
        self.assertEqual(reloaded.generation, "generation-two")
        self.assertEqual(len(self.systemctl.commands), command_count)

    def test_same_version_repack_is_an_atomic_runtime_update(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        repacked = make_plugin(self.root, "0.1.0", "repacked")
        first_state = self.manager.activate(first, "0.1.0", "generation-one")
        self.systemctl.gamescope_active = True
        self.systemctl.test_provider_active = True

        state = self.manager.activate(repacked, "0.1.0", "generation-two")

        self.assertEqual(state.active_version, "0.1.0")
        self.assertEqual(state.previous_version, "0.1.0")
        self.assertNotEqual(state.active_runtime, first_state.active_runtime)
        self.assertEqual(state.previous_runtime, first_state.active_runtime)
        self.assertEqual(
            (self.paths.versions / state.active_runtime / "bin/mangoapp").read_text(
                encoding="utf-8"
            ),
            "#!/bin/sh\n# repacked\nexit 0\n",
        )
        self.assertTrue(
            (self.paths.versions / state.previous_runtime / "bin/mangoapp").is_file()
        )
        self.assertIn(
            ("systemctl", "--user", "restart", "gamescope-mangoapp.service"),
            self.systemctl.commands,
        )
        self.assertIn(
            (
                "systemctl",
                "--user",
                "restart",
                "mango-overlay-test-provider.service",
            ),
            self.systemctl.commands,
        )

    def test_legacy_version_directory_migrates_during_repack(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        repacked = make_plugin(self.root, "0.1.0", "repacked")
        original = self.manager.activate(first, "0.1.0", "generation-one")
        legacy_runtime = self.paths.versions / "0.1.0"
        (self.paths.versions / original.active_runtime).rename(legacy_runtime)
        self.manager._write_json(
            self.paths.state_file,
            {
                "schema": 1,
                "active_version": "0.1.0",
                "previous_version": None,
                "generation": "generation-one",
            },
        )

        state = self.manager.activate(repacked, "0.1.0", "generation-two")

        self.assertEqual(state.active_version, "0.1.0")
        self.assertEqual(state.previous_version, "0.1.0")
        self.assertEqual(state.previous_runtime, "0.1.0")
        self.assertTrue(legacy_runtime.is_dir())
        self.assertTrue((self.paths.versions / state.active_runtime).is_dir())

    def test_failed_same_version_repack_restores_prior_runtime(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        repacked = make_plugin(self.root, "0.1.0", "repacked")
        expected = self.manager.activate(first, "0.1.0", "generation-one")
        self.systemctl.gamescope_active = True
        self.systemctl.fail_mango_restarts = 1

        with self.assertRaises(LifecycleError):
            self.manager.activate(repacked, "0.1.0", "generation-two")

        self.assertEqual(self.manager.status(), expected)
        self.assertEqual(
            sorted(path.name for path in self.paths.versions.iterdir()),
            [expected.active_runtime],
        )

    def test_update_keeps_one_rollback_version(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        second = make_plugin(self.root, "0.2.0", "second")
        self.manager.activate(first, "0.1.0", "generation-one")
        self.systemctl.gamescope_active = True
        self.systemctl.test_provider_active = True

        state = self.manager.activate(second, "0.2.0", "generation-two")

        self.assertEqual(state.active_version, "0.2.0")
        self.assertEqual(state.previous_version, "0.1.0")
        self.assertIn(
            ("systemctl", "--user", "restart", "gamescope-mangoapp.service"),
            self.systemctl.commands,
        )
        self.assertIn(
            (
                "systemctl",
                "--user",
                "restart",
                "mango-overlay-test-provider.service",
            ),
            self.systemctl.commands,
        )
        self.assertEqual(
            sorted(path.name for path in self.paths.versions.iterdir()),
            sorted((state.previous_runtime, state.active_runtime)),
        )

    def test_failed_update_restores_previous_version(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        second = make_plugin(self.root, "0.2.0", "broken")
        self.manager.activate(first, "0.1.0", "generation-one")
        self.systemctl.gamescope_active = True
        self.systemctl.fail_mango_restarts = 1

        with self.assertRaises(LifecycleError):
            self.manager.activate(second, "0.2.0", "generation-two")

        state = self.manager.status()
        self.assertEqual(state.active_version, "0.1.0")
        self.assertIsNone(state.previous_version)
        self.assertFalse(self.paths.transaction_file.exists())
        self.assertFalse((self.paths.versions / "0.2.0").exists())
        self.assertEqual(
            (self.paths.libexec_root / "launcher.py").read_text(encoding="utf-8"),
            "# launcher first\n",
        )

    def test_failed_initial_install_removes_enabled_integration(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.systemctl.fail_integration_enables = 1

        with self.assertRaises(LifecycleError):
            self.manager.activate(plugin, "0.1.0", "generation-one")

        self.assertIsNone(self.manager.status().active_version)
        self.assertFalse((self.paths.versions / "0.1.0").exists())
        self.assertFalse(self.paths.libexec_root.exists())
        self.assertFalse(self.paths.mangoapp_dropin.exists())
        for unit in self.manager._unit_paths():
            self.assertFalse(unit.exists())
        self.assertIn(
            (
                "systemctl",
                "--user",
                "disable",
                "--now",
                "mango-overlayd.socket",
                "mango-overlay-cleanup.timer",
            ),
            self.systemctl.commands,
        )

    def test_new_generation_cancels_pending_uninstall(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        second = make_plugin(self.root, "0.2.0", "second")
        self.manager.activate(first, "0.1.0", "generation-one")
        self.manager.mark_pending_uninstall(first, "generation-one")
        self.assertTrue(self.paths.pending_uninstall_file.exists())
        self.assertFalse(self.systemctl.cleanup_started_while_locked)

        self.manager.activate(second, "0.2.0", "generation-two")

        self.assertFalse(self.paths.pending_uninstall_file.exists())
        self.assertFalse(self.manager.finalize_pending_uninstall())
        self.assertIn(
            (
                "systemctl",
                "--user",
                "stop",
                "mango-overlay-cleanup.timer",
            ),
            self.systemctl.commands,
        )

    def test_mark_pending_uninstall_does_not_wait_for_cleanup_service(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        self.systemctl.cleanup_service_delay = 0.5

        started = time.monotonic()
        self.manager.mark_pending_uninstall(plugin, "generation-one")
        elapsed = time.monotonic() - started

        self.assertLess(elapsed, 0.25)
        self.assertIn(
            (
                "systemctl",
                "--user",
                "--no-block",
                "restart",
                "mango-overlay-cleanup.timer",
            ),
            self.systemctl.commands,
        )
        self.assertNotIn(
            (
                "systemctl",
                "--user",
                "start",
                "mango-overlay-cleanup.service",
            ),
            self.systemctl.commands,
        )

    def test_replacement_plugin_cancels_pending_before_backend_activation(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        expected = self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        shutil.rmtree(plugin)
        make_plugin(self.root, "0.2.0", "replacement")
        self.now += UNINSTALL_GRACE_SECONDS + 0.1

        self.assertFalse(self.manager.finalize_pending_uninstall(token))
        self.assertEqual(self.manager.status(), expected)
        self.assertFalse(self.paths.pending_uninstall_file.exists())

    def test_partial_replacement_at_the_same_path_is_ambiguous(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        expected = self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        shutil.rmtree(plugin)
        plugin.mkdir()
        self.now += UNINSTALL_GRACE_SECONDS + 0.1

        self.assertFalse(self.manager.finalize_pending_uninstall(token))
        self.assertEqual(self.manager.status(), expected)
        self.assertTrue(self.paths.pending_uninstall_file.exists())
        self.assertTrue(self.paths.mangoapp_dropin.exists())

    def test_valid_replacement_at_the_same_path_cancels_pending(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        expected = self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        replacement = make_plugin(self.root, "0.2.0", "replacement")
        shutil.rmtree(plugin)
        replacement.rename(plugin)

        self.assertFalse(self.manager.finalize_pending_uninstall(token))
        self.assertEqual(self.manager.status(), expected)
        self.assertFalse(self.paths.pending_uninstall_file.exists())

    def test_legacy_pending_record_finalizes_after_the_old_directory_is_gone(
        self,
    ) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        pending = json.loads(
            self.paths.pending_uninstall_file.read_text(encoding="utf-8")
        )
        pending.pop("plugin_device")
        pending.pop("plugin_inode")
        self.manager._write_json(self.paths.pending_uninstall_file, pending)

        self.assertFalse(self.manager.finalize_pending_uninstall(token))
        shutil.rmtree(plugin)
        self.now += UNINSTALL_GRACE_SECONDS + 0.1

        self.assertTrue(self.manager.finalize_pending_uninstall(token))

    def test_untrusted_plugins_root_blocks_final_cleanup(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        shutil.rmtree(plugin)
        self.root.chmod(0o777)
        self.now += UNINSTALL_GRACE_SECONDS + 0.1

        with self.assertRaisesRegex(LifecycleError, "plugins root is unsafe"):
            self.manager.finalize_pending_uninstall(token)

        self.assertTrue(self.paths.pending_uninstall_file.exists())
        self.assertTrue(self.paths.mangoapp_dropin.exists())

    def test_replacement_scan_is_bounded(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        shutil.rmtree(plugin)
        for index in range(MAX_PLUGIN_DIRECTORIES):
            (self.root / f"unrelated-{index}").mkdir()
        self.now += UNINSTALL_GRACE_SECONDS + 0.1

        with self.assertRaisesRegex(LifecycleError, "too many entries"):
            self.manager.finalize_pending_uninstall(token)

        self.assertTrue(self.paths.pending_uninstall_file.exists())
        self.assertTrue(self.paths.mangoapp_dropin.exists())

    def test_final_uninstall_requires_grace_and_plugin_absence(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        decky_roots = (
            self.home / "homebrew/settings/mango-overlay-decky",
            self.home / "homebrew/data/mango-overlay-decky",
            self.home / "homebrew/logs/mango-overlay-decky",
        )
        for root in decky_roots:
            root.mkdir(parents=True)
            (root / "owned").write_text("owned", encoding="utf-8")
        sibling = self.home / "homebrew/settings/another-plugin/keep"
        sibling.parent.mkdir(parents=True)
        sibling.write_text("keep", encoding="utf-8")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        self.assertFalse(self.manager.finalize_pending_uninstall(token))

        shutil.rmtree(plugin)
        self.assertFalse(self.manager.finalize_pending_uninstall(token))
        self.now += UNINSTALL_GRACE_SECONDS + 0.1
        self.systemctl.gamescope_active = True

        self.assertTrue(self.manager.finalize_pending_uninstall(token))
        self.assertFalse(self.paths.mangoapp_dropin.exists())
        self.assertFalse(self.paths.data_root.exists())
        self.assertFalse(self.paths.libexec_root.exists())
        for root in decky_roots:
            self.assertFalse(root.exists())
        self.assertEqual(sibling.read_text(encoding="utf-8"), "keep")
        self.assertIn(
            ("systemctl", "--user", "restart", "gamescope-mangoapp.service"),
            self.systemctl.commands,
        )

    def test_uninstall_record_must_match_active_generation(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        with self.assertRaises(LifecycleError):
            self.manager.mark_pending_uninstall(plugin, "stale-generation")

        state_mode = stat.S_IMODE(self.paths.state_file.stat().st_mode)
        self.assertEqual(state_mode, 0o600)

    def test_plugin_package_version_must_match_decky_version(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        (plugin / "package.json").write_text(
            json.dumps({"name": "mango-overlay-decky", "version": "0.2.0"}),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(LifecycleError, "identity differs"):
            self.manager.activate(plugin, "0.1.0", "generation-one")

    def test_test_provider_and_broker_controls_do_not_change_install_state(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        expected = self.manager.activate(plugin, "0.1.0", "generation-one")

        self.manager.set_test_provider(True)
        self.assertTrue(self.manager.test_provider_active())
        self.manager.restart_broker()
        self.manager.set_test_provider(False)

        self.assertFalse(self.manager.test_provider_active())
        self.assertEqual(self.manager.status(), expected)
        self.assertIn(
            ("systemctl", "--user", "restart", "mango-overlayd.service"),
            self.systemctl.commands,
        )

    def test_emergency_restore_uses_system_mangoapp_without_uninstalling(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        expected = self.manager.activate(plugin, "0.1.0", "generation-one")
        self.systemctl.gamescope_active = True

        self.manager.restore_system_mangoapp()

        self.assertFalse(self.paths.mangoapp_dropin.exists())
        self.assertEqual(self.manager.status(), expected)
        self.assertTrue(
            (self.paths.versions / expected.active_runtime / "bin/mangoapp").exists()
        )
        self.assertIn(
            ("systemctl", "--user", "daemon-reload"),
            self.systemctl.commands,
        )
        self.assertIn(
            ("systemctl", "--user", "restart", "gamescope-mangoapp.service"),
            self.systemctl.commands,
        )

    def test_emergency_restore_does_not_start_an_inactive_mangoapp(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        command_count = len(self.systemctl.commands)

        self.manager.restore_system_mangoapp()

        commands = self.systemctl.commands[command_count:]
        self.assertNotIn(
            ("systemctl", "--user", "restart", "gamescope-mangoapp.service"),
            commands,
        )

    def test_interrupted_update_recovers_prior_state(self) -> None:
        first = make_plugin(self.root, "0.1.0", "first")
        second = make_plugin(self.root, "0.2.0", "second")
        self.manager.activate(first, "0.1.0", "generation-one")
        self.systemctl.crash_on_broker_restart = True

        with self.assertRaises(KeyboardInterrupt):
            self.manager.activate(second, "0.2.0", "generation-two")
        self.assertTrue(self.paths.transaction_file.exists())
        self.assertEqual(self.manager.status().active_version, "0.2.0")

        recovered = self.manager.activate(first, "0.1.0", "generation-three")
        self.assertEqual(recovered.active_version, "0.1.0")
        self.assertFalse(self.paths.transaction_file.exists())
        self.assertEqual(
            (self.paths.libexec_root / "launcher.py").read_text(encoding="utf-8"),
            "# launcher first\n",
        )

    def test_uninstall_recovers_an_interrupted_initial_install(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.systemctl.crash_on_broker_restart = True

        with self.assertRaises(KeyboardInterrupt):
            self.manager.activate(plugin, "0.1.0", "generation-one")
        self.assertTrue(self.paths.transaction_file.exists())
        recovery_start = len(self.systemctl.commands)

        with self.assertRaisesRegex(LifecycleError, "No active runtime"):
            self.manager.mark_pending_uninstall(plugin, "generation-one")

        recovery_commands = self.systemctl.commands[recovery_start:]
        self.assertNotIn(
            ("systemctl", "--user", "try-restart", "mango-overlayd.service"),
            recovery_commands,
        )
        self.assertFalse(self.paths.transaction_file.exists())
        self.assertIsNone(self.manager.status().active_version)
        self.assertFalse((self.paths.versions / "0.1.0").exists())
        self.assertFalse(self.paths.mangoapp_dropin.exists())
        for unit in self.manager._unit_paths():
            self.assertFalse(unit.exists())

    def test_unsafe_cleanup_data_does_not_block_system_restore(self) -> None:
        plugin = make_plugin(self.root, "0.1.0", "first")
        self.manager.activate(plugin, "0.1.0", "generation-one")
        token = self.manager.mark_pending_uninstall(plugin, "generation-one")
        shutil.rmtree(plugin)
        self.now += UNINSTALL_GRACE_SECONDS + 0.1
        self.systemctl.gamescope_active = True
        external = self.root / "external"
        external.write_text("keep", encoding="utf-8")
        (self.paths.data_root / "unsafe-link").symlink_to(external)

        with self.assertRaises(LifecycleError):
            self.manager.finalize_pending_uninstall(token)

        self.assertFalse(self.paths.mangoapp_dropin.exists())
        self.assertTrue(external.exists())
        self.assertTrue(
            (self.paths.systemd_user_root / "mango-overlay-cleanup.timer").exists()
        )


if __name__ == "__main__":
    unittest.main()
