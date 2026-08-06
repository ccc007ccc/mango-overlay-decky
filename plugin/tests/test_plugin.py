from __future__ import annotations

import asyncio
import importlib.util
import os
import sys
import subprocess
import threading
import types
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, Mock, patch

from mango_overlay_decky.lifecycle import LifecycleError, LifecycleState


def load_plugin(fake_decky: types.ModuleType):
    module_path = Path(__file__).parents[1] / "main.py"
    spec = importlib.util.spec_from_file_location("mango_overlay_test_plugin", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {"decky": fake_decky}):
        spec.loader.exec_module(module)
    return module


def fake_decky() -> types.ModuleType:
    module = types.ModuleType("decky")
    module.DECKY_PLUGIN_DIR = "/plugin"  # type: ignore[attr-defined]
    module.DECKY_PLUGIN_VERSION = "0.1.0"  # type: ignore[attr-defined]
    module.DECKY_USER_HOME = "/home/deck"  # type: ignore[attr-defined]
    module.logger = types.SimpleNamespace(  # type: ignore[attr-defined]
        exception=Mock(),
        info=Mock(),
        warning=Mock(),
    )
    return module


class PluginTests(unittest.IsolatedAsyncioTestCase):
    async def test_main_activates_one_generation(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        manager = types.SimpleNamespace(
            activate=Mock(return_value=LifecycleState("0.1.0", None, "a" * 32)),
            status=Mock(),
        )
        with (
            patch.object(module, "LifecycleManager", return_value=manager),
            patch.object(module.secrets, "token_hex", return_value="a" * 32),
        ):
            plugin = module.Plugin()
            await plugin._main()

        manager.activate.assert_called_once_with(
            Path("/plugin"), "0.1.0", "a" * 32
        )

    async def test_activation_failure_is_reported_without_controller_noise(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        manager = types.SimpleNamespace(
            activate=Mock(
                side_effect=LifecycleError(
                    "systemctl_failed", "user manager is unavailable"
                )
            ),
            status=Mock(return_value=LifecycleState(None, None, "")),
            test_provider_active=Mock(return_value=False),
        )
        with patch.object(module, "LifecycleManager", return_value=manager):
            plugin = module.Plugin()
            await plugin._main()

        plugin._run_control = Mock(side_effect=RuntimeError("controller noise"))
        status = await plugin.get_status()

        self.assertEqual(status["error"], "激活失败：user manager is unavailable")
        plugin._run_control.assert_not_called()

    async def test_unload_has_no_lifecycle_side_effect(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._manager = Mock()

        await asyncio.wait_for(plugin._unload(), timeout=0.01)

        self.assertEqual(plugin._manager.mock_calls, [])

    async def test_uninstall_only_marks_active_generation(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._paths = module.LifecyclePaths.for_home(Path("/home/deck"))
        plugin._manager = types.SimpleNamespace(
            mark_pending_uninstall=Mock(return_value="b" * 32)
        )
        plugin._generation = "a" * 32
        plugin._plugin_root = Path("/plugin")

        await plugin._uninstall()

        plugin._manager.mark_pending_uninstall.assert_called_once_with(
            Path("/plugin"), "a" * 32
        )

    async def test_uninstall_records_pending_on_event_loop_thread(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        callback_thread = threading.get_ident()
        mark_threads: list[int] = []
        plugin._manager = types.SimpleNamespace(
            mark_pending_uninstall=Mock(
                side_effect=lambda *_: (
                    mark_threads.append(threading.get_ident()),
                    "b" * 32,
                )[1]
            )
        )
        plugin._generation = "a" * 32
        plugin._plugin_root = Path("/plugin")

        await plugin._uninstall()

        self.assertEqual(mark_threads, [callback_thread])

    async def test_uninstall_accepts_a_recovered_partial_install(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._manager = types.SimpleNamespace(
            mark_pending_uninstall=Mock(
                side_effect=LifecycleError(
                    "not_installed", "No active runtime is installed"
                )
            )
        )
        plugin._generation = "a" * 32
        plugin._plugin_root = Path("/plugin")

        await plugin._uninstall()

        decky.logger.info.assert_called_with(
            "Mango Overlay uninstall callback has no active installation"
        )

    async def test_uninstall_ignores_a_generation_that_failed_activation(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._manager = types.SimpleNamespace(
            mark_pending_uninstall=Mock(
                side_effect=LifecycleError(
                    "stale_generation",
                    "The uninstall callback does not own the active generation",
                )
            )
        )
        plugin._generation = "a" * 32
        plugin._plugin_root = Path("/plugin")

        await plugin._uninstall()

        decky.logger.info.assert_called_with(
            "Mango Overlay uninstall callback has no active installation"
        )

    async def test_status_reports_plugin_and_runtime_versions(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._manager = types.SimpleNamespace(
            status=Mock(
                return_value=LifecycleState("0.2.0", "0.1.0", "a" * 32)
            ),
            test_provider_active=Mock(return_value=False),
        )

        plugin._run_control = Mock(
            return_value={"enabled": True, "applications": []}
        )
        status = await plugin.get_status()

        self.assertEqual(
            status,
            {
                "plugin_version": "0.1.0",
                "active_version": "0.2.0",
                "previous_version": "0.1.0",
                "broker": {"enabled": True, "applications": []},
                "error": None,
                "test_canvas": False,
            },
        )

    async def test_control_mutations_use_the_stable_launcher(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._paths = module.LifecyclePaths.for_home(Path("/home/deck"))
        plugin._run_control = Mock(return_value={"enabled": False})

        result = await plugin.set_enabled(False)
        await plugin.set_require_approval(True)
        await plugin.set_provider_policy("dev.example", True, False, -2)
        await plugin.set_provider_position("dev.example", 0)

        self.assertEqual(result, {"enabled": False})
        self.assertEqual(
            plugin._run_control.call_args_list,
            [
                unittest.mock.call("set-enabled", "false"),
                unittest.mock.call("set-require-approval", "true"),
                unittest.mock.call(
                    "set-provider", "dev.example", "true", "false", "-2"
                ),
                unittest.mock.call(
                    "set-provider-position", "dev.example", "0"
                ),
            ],
        )

    async def test_status_reports_controller_failure_without_hiding_runtime(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._paths = module.LifecyclePaths.for_home(Path("/home/deck"))
        plugin._manager = types.SimpleNamespace(
            status=Mock(
                return_value=LifecycleState("0.1.0", None, "a" * 32)
            ),
            test_provider_active=Mock(return_value=True),
        )
        plugin._run_control = Mock(side_effect=RuntimeError("broker unavailable"))

        status = await plugin.get_status()

        self.assertIsNone(status["broker"])
        self.assertEqual(status["active_version"], "0.1.0")
        self.assertEqual(status["error"], "broker unavailable")
        self.assertTrue(status["test_canvas"])

    async def test_diagnostic_controls_delegate_to_lifecycle_manager(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._manager = types.SimpleNamespace(
            set_test_provider=Mock(),
            test_provider_active=Mock(return_value=True),
            restart_broker=Mock(),
        )

        active = await plugin.set_test_canvas(True)
        await plugin.restart_broker()

        self.assertTrue(active)
        plugin._manager.set_test_provider.assert_called_once_with(True)
        plugin._manager.restart_broker.assert_called_once_with()

    def test_run_control_does_not_use_a_shell(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._paths = module.LifecyclePaths.for_home(Path("/home/deck"))
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='{"enabled":true}', stderr=""
        )
        with patch.object(module.subprocess, "run", return_value=completed) as run:
            status = plugin._run_control("status")

        self.assertEqual(status, {"enabled": True})
        run.assert_called_once()
        self.assertEqual(
            run.call_args.args[0],
            [
                "/home/deck/.local/libexec/mango-overlay-decky/launcher.py",
                "controller",
                "status",
            ],
        )
        self.assertFalse(run.call_args.kwargs["check"])
        self.assertTrue(run.call_args.kwargs["capture_output"])
        self.assertTrue(run.call_args.kwargs["text"])
        self.assertEqual(run.call_args.kwargs["timeout"], 5)
        self.assertNotIn("shell", run.call_args.kwargs)

    def test_run_control_targets_the_decky_user_runtime(self) -> None:
        decky = fake_decky()
        module = load_plugin(decky)
        plugin = module.Plugin()
        plugin._paths = module.LifecyclePaths.for_home(Path("/home/deck"))
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='{"enabled":true}', stderr=""
        )
        decky_environment = {
            "HOME": "/home/deck",
            "XDG_RUNTIME_DIR": "/run/user/0",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/0/bus",
        }
        with (
            patch.dict(os.environ, decky_environment, clear=True),
            patch("os.geteuid", return_value=1000),
            patch.object(module.subprocess, "run", return_value=completed) as run,
        ):
            plugin._run_control("status")

        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["XDG_RUNTIME_DIR"], "/run/user/1000")
        self.assertEqual(
            environment["DBUS_SESSION_BUS_ADDRESS"],
            "unix:path=/run/user/1000/bus",
        )


if __name__ == "__main__":
    unittest.main()
