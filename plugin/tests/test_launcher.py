from __future__ import annotations

import json
import os
import signal
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from mango_overlay_decky import launcher


class LauncherTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="mango-overlay-launcher-")
        self.home = Path(self.temporary.name)
        self.version = "0.1.0"
        self.runtime = (
            self.home
            / ".local/share/mango-overlay-decky/runtime/versions"
            / self.version
        )
        for name in (
            "mangoapp",
            "mango-overlayd",
            "mango-overlayctl",
            "mango-overlay-test-provider",
        ):
            binary = self.runtime / "bin" / name
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            binary.chmod(0o755)
        for relative in launcher.DESKTOP_RUNTIME_FILES:
            path = self.runtime / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"desktop:{relative}\n", encoding="utf-8")
            path.chmod(0o644)
        state = self.home / ".local/state/mango-overlay-decky/install.json"
        state.parent.mkdir(parents=True, exist_ok=True)
        state.write_text(
            json.dumps({"schema": 1, "active_version": self.version}),
            encoding="utf-8",
        )
        state.chmod(0o600)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_active_binary_resolves_each_runtime_role(self) -> None:
        with patch.object(launcher.Path, "home", return_value=self.home):
            self.assertEqual(launcher.active_runtime(), self.runtime)
            self.assertEqual(
                launcher.active_binary("mangoapp"), self.runtime / "bin/mangoapp"
            )
            self.assertEqual(
                launcher.active_binary("broker"), self.runtime / "bin/mango-overlayd"
            )
            self.assertEqual(
                launcher.active_binary("controller"),
                self.runtime / "bin/mango-overlayctl",
            )
            self.assertEqual(
                launcher.active_binary("test-provider"),
                self.runtime / "bin/mango-overlay-test-provider",
            )

    def test_content_addressed_runtime_is_selected_from_state(self) -> None:
        revision = "a" * 64
        revision_runtime = (
            self.home
            / ".local/share/mango-overlay-decky/runtime/versions"
            / revision
        )
        binary = revision_runtime / "bin/mangoapp"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        binary.chmod(0o755)
        state = self.home / ".local/state/mango-overlay-decky/install.json"
        state.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "active_version": self.version,
                    "active_runtime": revision,
                }
            ),
            encoding="utf-8",
        )

        with patch.object(launcher.Path, "home", return_value=self.home):
            self.assertEqual(launcher.active_binary("mangoapp"), binary)

    def test_unsafe_runtime_binary_is_rejected(self) -> None:
        binary = self.runtime / "bin/mango-overlayctl"
        binary.chmod(0o777)
        with patch.object(launcher.Path, "home", return_value=self.home):
            self.assertIsNone(launcher.active_binary("controller"))
        self.assertEqual(stat.S_IMODE(binary.stat().st_mode), 0o777)

    def test_broker_receives_only_the_owned_policy_path(self) -> None:
        binary = self.runtime / "bin/mango-overlayd"
        inherited_path = "/existing/library/path"
        with (
            patch.object(launcher.Path, "home", return_value=self.home),
            patch.object(launcher, "active_binary", return_value=binary),
            patch.object(sys, "argv", ["launcher.py", "broker"]),
            patch.dict(os.environ, {"LD_LIBRARY_PATH": inherited_path}, clear=True),
            patch.object(os, "execve", side_effect=RuntimeError("exec")) as execute,
        ):
            with self.assertRaisesRegex(RuntimeError, "exec"):
                launcher.main()
        execute.assert_called_once()
        executed_binary, arguments, environment = execute.call_args.args
        self.assertEqual(executed_binary, binary)
        self.assertEqual(
            arguments,
            ["mango-overlayd", "--policy-file", str(self.home / ".config/mango-overlay-decky/policy.fb")],
        )
        self.assertEqual(
            environment["LD_LIBRARY_PATH"],
            os.pathsep.join((str(self.runtime / "lib"), inherited_path)),
        )

    def test_controller_arguments_are_forwarded_without_a_shell(self) -> None:
        binary = self.runtime / "bin/mango-overlayctl"
        arguments = ["launcher.py", "controller", "set-enabled", "false"]
        with (
            patch.object(launcher.Path, "home", return_value=self.home),
            patch.object(launcher, "active_binary", return_value=binary),
            patch.object(sys, "argv", arguments),
            patch.dict(os.environ, {}, clear=True),
            patch.object(os, "execve", side_effect=RuntimeError("exec")) as execute,
        ):
            with self.assertRaisesRegex(RuntimeError, "exec"):
                launcher.main()
        execute.assert_called_once()
        executed_binary, executed_arguments, environment = execute.call_args.args
        self.assertEqual(executed_binary, binary)
        self.assertEqual(executed_arguments, ["mango-overlayctl", "set-enabled", "false"])
        self.assertEqual(environment["LD_LIBRARY_PATH"], str(self.runtime / "lib"))

    def test_system_mangoapp_fallback_preserves_the_environment(self) -> None:
        environment = {"PATH": "/usr/bin", "LD_LIBRARY_PATH": "/steam/runtime"}
        with (
            patch.object(launcher, "active_binary", return_value=None),
            patch.object(sys, "argv", ["launcher.py", "mangoapp"]),
            patch.dict(os.environ, environment, clear=True),
            patch.object(os, "execve", side_effect=RuntimeError("exec")) as execute,
        ):
            with self.assertRaisesRegex(RuntimeError, "exec"):
                launcher.main()
        execute.assert_called_once_with(
            Path("/usr/bin/mangoapp"),
            ["mangoapp"],
            environment,
        )

    def test_managed_mangoapp_exit_falls_back_without_private_library_path(self) -> None:
        binary = self.runtime / "bin/mangoapp"
        process = Mock()
        process.wait.return_value = 1
        environment = {"PATH": "/usr/bin", "LD_LIBRARY_PATH": "/steam/runtime"}
        with (
            patch.object(launcher, "active_binary", return_value=binary),
            patch.object(sys, "argv", ["launcher.py", "mangoapp"]),
            patch.dict(os.environ, environment, clear=True),
            patch.object(launcher.subprocess, "Popen", return_value=process) as popen,
            patch.object(launcher.signal, "signal", return_value=signal.SIG_DFL),
            patch.object(os, "execve", side_effect=RuntimeError("fallback")) as execute,
        ):
            with self.assertRaisesRegex(RuntimeError, "fallback"):
                launcher.main()

        popen.assert_called_once_with(
            ["mangoapp"],
            executable=binary,
            env={
                "PATH": "/usr/bin",
                "LD_LIBRARY_PATH": os.pathsep.join(
                    (str(self.runtime / "lib"), "/steam/runtime")
                ),
            },
        )
        execute.assert_called_once_with(
            Path("/usr/bin/mangoapp"),
            ["mangoapp"],
            environment,
        )

    def test_normal_mangoapp_stop_does_not_trigger_fallback(self) -> None:
        binary = self.runtime / "bin/mangoapp"
        handlers: dict[signal.Signals, object] = {}
        forwarded: list[signal.Signals] = []

        def install_handler(number: signal.Signals, handler: object) -> object:
            previous = handlers.get(number, signal.SIG_DFL)
            handlers[number] = handler
            return previous

        process = Mock()
        process.poll.return_value = None
        process.send_signal.side_effect = forwarded.append

        def wait() -> int:
            handler = handlers[signal.SIGTERM]
            assert callable(handler)
            handler(signal.SIGTERM, None)
            return -signal.SIGTERM

        process.wait.side_effect = wait
        with (
            patch.object(launcher, "active_binary", return_value=binary),
            patch.object(sys, "argv", ["launcher.py", "mangoapp"]),
            patch.dict(os.environ, {}, clear=True),
            patch.object(launcher.subprocess, "Popen", return_value=process),
            patch.object(launcher.signal, "signal", side_effect=install_handler),
            patch.object(os, "execve") as execute,
        ):
            self.assertEqual(launcher.main(), 0)

        self.assertEqual(forwarded, [signal.SIGTERM])
        execute.assert_not_called()

    def test_missing_broker_does_not_fall_back_to_mangoapp(self) -> None:
        with (
            patch.object(launcher, "active_binary", return_value=None),
            patch.object(sys, "argv", ["launcher.py", "broker"]),
            patch.object(os, "execve") as execute,
        ):
            self.assertEqual(launcher.main(), 69)
        execute.assert_not_called()

    def test_desktop_launch_uses_the_dual_abi_runtime_without_a_shell(self) -> None:
        inherited = {
            "PATH": "/usr/bin",
            "LD_LIBRARY_PATH": "/steam/runtime",
            "LD_PRELOAD": "/usr/lib/libMangoHud_shim.so libcapture.so",
            "MANGOHUD_CONFIG": "fps_limit=60,no_display=1",
            "DISABLE_MANGOHUD": "1",
            "VK_INSTANCE_LAYERS": "system-layer",
            "VK_LOADER_LAYERS_DISABLE": "~implicit~",
            "VK_LOADER_LAYERS_ENABLE": "system-layer",
        }
        arguments = ["launcher.py", "desktop", "--", "game", "--flag"]
        with (
            patch.object(launcher, "active_runtime", return_value=self.runtime),
            patch.object(sys, "argv", arguments),
            patch.dict(os.environ, inherited, clear=True),
            patch.object(os, "execvpe", side_effect=RuntimeError("exec")) as execute,
        ):
            with self.assertRaisesRegex(RuntimeError, "exec"):
                launcher.main()

        execute.assert_called_once()
        executable, executed_arguments, environment = execute.call_args.args
        library_directory = f"{self.runtime}/$LIB"
        self.assertEqual(executable, "game")
        self.assertEqual(executed_arguments, ["game", "--flag"])
        self.assertEqual(environment["MANGOHUD"], "1")
        self.assertEqual(environment["MANGOHUD_CONFIGFILE"], "/dev/null")
        self.assertEqual(environment["MANGOHUD_CONFIG"], "fps_limit=60,no_display=1")
        self.assertEqual(
            environment["MANGO_OVERLAY_SOCKET"],
            f"/run/user/{os.geteuid()}/mango-overlay-decky.sock",
        )
        self.assertEqual(
            environment["MANGOHUD_OPENGL_LIBS"],
            f"{library_directory}/libMangoHud_opengl.so",
        )
        self.assertEqual(
            environment["VK_IMPLICIT_LAYER_PATH"],
            str(self.runtime / "share/vulkan/implicit_layer.d"),
        )
        self.assertEqual(
            environment["LD_LIBRARY_PATH"],
            os.pathsep.join((library_directory, "/steam/runtime")),
        )
        self.assertEqual(
            environment["LD_PRELOAD"],
            os.pathsep.join(
                (f"{library_directory}/libMangoHud_shim.so", "libcapture.so")
            ),
        )
        for name in (
            "DISABLE_MANGOHUD",
            "VK_INSTANCE_LAYERS",
            "VK_LOADER_LAYERS_DISABLE",
            "VK_LOADER_LAYERS_ENABLE",
        ):
            self.assertNotIn(name, environment)

    def test_desktop_launch_defaults_to_provider_only_canvas(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            environment = launcher.desktop_environment(self.runtime, {})
        self.assertIsNotNone(environment)
        assert environment is not None
        self.assertEqual(environment["MANGOHUD_CONFIG"], "no_display=1")

    def test_desktop_launch_rejects_an_incomplete_runtime(self) -> None:
        (self.runtime / "lib32/libMangoHud_shim.so").unlink()
        with (
            patch.object(launcher, "active_runtime", return_value=self.runtime),
            patch.object(sys, "argv", ["launcher.py", "desktop", "--", "game"]),
            patch.object(os, "execvpe") as execute,
        ):
            self.assertEqual(launcher.main(), 69)
        execute.assert_not_called()

    def test_desktop_launch_requires_a_command_separator(self) -> None:
        with (
            patch.object(sys, "argv", ["launcher.py", "desktop", "game"]),
            patch.object(os, "execvpe") as execute,
        ):
            self.assertEqual(launcher.main(), 64)
        execute.assert_not_called()


if __name__ == "__main__":
    unittest.main()
