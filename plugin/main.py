from __future__ import annotations

import asyncio
import json
import os
import secrets
import subprocess
from pathlib import Path
from typing import Any

import decky

from mango_overlay_decky.lifecycle import (
    LifecycleError,
    LifecycleManager,
    LifecyclePaths,
    LifecycleRuntimeOperations,
)
from mango_overlay_decky.coordinator import (
    CoordinatorError,
    CoordinatorPaths,
    RuntimeClaim,
    RuntimeCoordinator,
)


PRODUCT_ID = "mango-overlay-decky"


class Plugin:
    async def _main(self) -> None:
        self._activation_error: str | None = None
        self._generation = secrets.token_hex(16)
        self._plugin_root = Path(decky.DECKY_PLUGIN_DIR)
        self._paths = LifecyclePaths.for_home(Path(decky.DECKY_USER_HOME))
        self._manager = LifecycleManager(self._paths)
        try:
            prepare_candidate = getattr(
                self._manager, "prepare_runtime_candidate", None
            )
            if callable(prepare_candidate):
                self._candidate = await asyncio.to_thread(
                    prepare_candidate,
                    self._plugin_root,
                    decky.DECKY_PLUGIN_VERSION,
                )
                self._operations = LifecycleRuntimeOperations(
                    self._manager,
                    plugin_root=self._plugin_root,
                    plugin_version=decky.DECKY_PLUGIN_VERSION,
                    generation=self._generation,
                )
                self._coordinator = RuntimeCoordinator(
                    CoordinatorPaths.for_home(Path(decky.DECKY_USER_HOME)),
                    self._operations,
                )
                outcome = await asyncio.to_thread(
                    self._coordinator.register,
                    RuntimeClaim(PRODUCT_ID, self._generation, self._candidate),
                )
                if outcome.action in {"blocked", "failed"}:
                    detail = outcome.error or outcome.action
                    raise LifecycleError(
                        "coordinator_" + outcome.action,
                        f"Shared Mango core could not be selected: {detail}",
                    )
                self._state = await asyncio.to_thread(self._manager.status)
            else:
                # Compatibility for an older helper during an in-place Decky
                # update; the new packaged helper always takes the coordinator
                # path above.
                self._state = await asyncio.to_thread(
                    self._manager.activate,
                    self._plugin_root,
                    decky.DECKY_PLUGIN_VERSION,
                    self._generation,
                )
        except Exception as exception:
            self._activation_error = f"激活失败：{exception}"
            decky.logger.exception("Mango Overlay activation failed")
            return
        decky.logger.info(
            "Mango Overlay runtime %s is active",
            self._state.active_version,
        )

    async def _unload(self) -> None:
        decky.logger.info("Mango Overlay Decky backend unloaded")

    async def _uninstall(self) -> None:
        manager = getattr(self, "_manager", None)
        generation = getattr(self, "_generation", None)
        plugin_root = getattr(self, "_plugin_root", None)
        if manager is None or generation is None or plugin_root is None:
            decky.logger.warning(
                "Mango Overlay uninstall callback has no active generation"
            )
            return
        try:
            coordinator_token: str | None = None
            coordinator = getattr(self, "_coordinator", None)
            if coordinator is not None:
                try:
                    coordinator_token = coordinator.mark_pending_removal(
                        PRODUCT_ID, generation
                    )
                except CoordinatorError as error:
                    if error.code not in {"claim_not_found", "stale_generation"}:
                        raise
            if coordinator_token is None:
                token = manager.mark_pending_uninstall(plugin_root, generation)
            else:
                token = manager.mark_pending_uninstall(
                    plugin_root,
                    generation,
                    coordinator_token=coordinator_token,
                    coordinator_product_id=PRODUCT_ID,
                )
        except LifecycleError as error:
            if error.code in {"not_installed", "stale_generation"}:
                decky.logger.info(
                    "Mango Overlay uninstall callback has no active installation"
                )
                return
            raise
        decky.logger.info("Mango Overlay uninstall verification armed: %s", token)

    def _run_control(self, *arguments: str) -> dict[str, Any]:
        launcher = self._paths.libexec_root / "launcher.py"
        environment = os.environ.copy()
        runtime_directory = f"/run/user/{os.geteuid()}"
        environment["XDG_RUNTIME_DIR"] = runtime_directory
        environment["DBUS_SESSION_BUS_ADDRESS"] = (
            f"unix:path={runtime_directory}/bus"
        )
        result = subprocess.run(
            [str(launcher), "controller", *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
            env=environment,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or "mango-overlayctl failed"
            raise RuntimeError(detail[:512])
        if len(result.stdout) > 1024 * 1024:
            raise RuntimeError("mango-overlayctl returned too much data")
        try:
            status = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError("mango-overlayctl returned invalid JSON") from error
        if not isinstance(status, dict):
            raise RuntimeError("mango-overlayctl returned invalid status")
        return status

    async def get_status(self) -> dict[str, Any]:
        state, test_canvas = await asyncio.gather(
            asyncio.to_thread(self._manager.status),
            asyncio.to_thread(self._manager.test_provider_active),
        )
        activation_error = getattr(self, "_activation_error", None)
        if activation_error is not None:
            broker = None
            error = activation_error
        else:
            try:
                broker = await asyncio.to_thread(self._run_control, "status")
                error = None
            except (OSError, subprocess.SubprocessError, RuntimeError) as exception:
                broker = None
                error = str(exception)
        result: dict[str, Any] = {
            "plugin_version": decky.DECKY_PLUGIN_VERSION,
            "active_version": state.active_version,
            "previous_version": state.previous_version,
            "broker": broker,
            "error": error,
            "test_canvas": test_canvas,
        }
        coordinator = getattr(self, "_coordinator", None)
        if coordinator is not None:
            try:
                coordinator_status = await asyncio.to_thread(coordinator.status)
            except Exception as exception:
                result["coordinator"] = {"error": str(exception)[:512]}
            else:
                result["coordinator"] = {
                    "active_revision": coordinator_status.active_revision,
                    "known_good_revision": coordinator_status.known_good_revision,
                    "failed_revisions": list(coordinator_status.failed_revisions),
                    "verified_revisions": list(
                        getattr(coordinator_status, "verified_revisions", ())
                    ),
                    "last_error": coordinator_status.last_error,
                    "claim_count": len(coordinator_status.claims),
                }
        return result

    async def set_enabled(self, enabled: bool) -> dict[str, Any]:
        if type(enabled) is not bool:
            raise ValueError("enabled must be a boolean")
        return await asyncio.to_thread(
            self._run_control,
            "set-enabled",
            "true" if enabled else "false",
        )

    async def set_require_approval(self, required: bool) -> dict[str, Any]:
        if type(required) is not bool:
            raise ValueError("required must be a boolean")
        return await asyncio.to_thread(
            self._run_control,
            "set-require-approval",
            "true" if required else "false",
        )

    async def set_provider_policy(
        self,
        application_id: str,
        approved: bool,
        visible: bool,
        order: int,
    ) -> dict[str, Any]:
        if (
            not isinstance(application_id, str)
            or not application_id
            or len(application_id) > 128
            or type(approved) is not bool
            or type(visible) is not bool
            or type(order) is not int
            or order < -(2**31)
            or order >= 2**31
        ):
            raise ValueError("invalid provider policy")
        return await asyncio.to_thread(
            self._run_control,
            "set-provider",
            application_id,
            "true" if approved else "false",
            "true" if visible else "false",
            str(order),
        )

    async def set_provider_position(
        self,
        application_id: str,
        position: int,
    ) -> dict[str, Any]:
        if (
            not isinstance(application_id, str)
            or not application_id
            or len(application_id) > 128
            or type(position) is not int
            or position < 0
            or position >= 2**32
        ):
            raise ValueError("invalid provider position")
        return await asyncio.to_thread(
            self._run_control,
            "set-provider-position",
            application_id,
            str(position),
        )

    async def set_test_canvas(self, enabled: bool) -> bool:
        if type(enabled) is not bool:
            raise ValueError("enabled must be a boolean")
        await asyncio.to_thread(self._manager.set_test_provider, enabled)
        return await asyncio.to_thread(self._manager.test_provider_active)

    async def restart_broker(self) -> None:
        await asyncio.to_thread(self._manager.restart_broker)
