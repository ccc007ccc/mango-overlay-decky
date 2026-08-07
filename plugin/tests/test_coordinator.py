from __future__ import annotations

import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from mango_overlay_decky.coordinator import (
    CoordinatorError,
    RuntimeClaim,
    RuntimeCandidate,
    CoordinatorPaths,
    RuntimeCoordinator,
    select_candidate,
)


def candidate(version: str, marker: str, ref: str | None = None) -> RuntimeCandidate:
    return RuntimeCandidate.create(
        core_version=version,
        content_revision=marker * 64,
        runtime_ref=ref or f"runtime-{marker}",
    )


def claim(index: int, runtime: RuntimeCandidate, *, pending: bool = False) -> RuntimeClaim:
    return RuntimeClaim(
        product_id=f"product.{index}",
        generation=f"generation-{index:08d}",
        candidate=runtime,
        pending_removal=pending,
    )


class CoordinatorSelectionTests(unittest.TestCase):
    def test_a_hundred_claims_only_compare_bounded_metadata_and_select_highest(self) -> None:
        low = candidate("1.0.0", "a")
        high = candidate("2.0.0", "b")
        claims = [claim(index, low) for index in range(99)]
        claims.append(claim(99, high))

        selection = select_candidate(claims, active_revision=None)

        self.assertEqual(selection.action, "activate")
        self.assertIs(selection.candidate, high)
        self.assertEqual(selection.reason, "higher_or_replacement_candidate")

    def test_same_version_different_content_is_blocked_without_random_choice(self) -> None:
        first = candidate("2.0.0", "a")
        second = candidate("2.0.0", "b")

        selection = select_candidate(
            [claim(1, first), claim(2, second)],
            active_revision="c" * 64,
        )

        self.assertEqual(selection.action, "blocked")
        self.assertIsNone(selection.candidate)
        self.assertEqual(selection.reason, "same_version_conflict")

    def test_failed_highest_revision_falls_back_to_the_next_version(self) -> None:
        high = candidate("3.0.0", "c")
        fallback = candidate("2.0.0", "b")

        selection = select_candidate(
            [claim(1, high), claim(2, fallback)],
            active_revision="a" * 64,
            failed_revisions={high.content_revision},
        )

        self.assertEqual(selection.action, "activate")
        self.assertIs(selection.candidate, fallback)

    def test_a_lower_claim_cannot_downgrade_the_active_highest_revision(self) -> None:
        high = candidate("3.0.0", "c")
        low = candidate("2.0.0", "b")

        selection = select_candidate(
            [claim(1, high), claim(2, low)],
            active_revision=high.content_revision,
        )

        self.assertEqual(selection.action, "unchanged")
        self.assertIs(selection.candidate, high)

    def test_pending_removal_remains_active_until_explicit_confirmation(self) -> None:
        runtime = candidate("1.0.0", "a")

        selection = select_candidate(
            [claim(1, runtime, pending=True)],
            active_revision=runtime.content_revision,
        )

        self.assertEqual(selection.action, "unchanged")
        self.assertIs(selection.candidate, runtime)

    def test_invalid_or_conflicting_metadata_is_rejected_before_selection(self) -> None:
        with self.assertRaisesRegex(CoordinatorError, "Core version"):
            RuntimeCandidate.create(
                core_version="2.0",
                content_revision="a" * 64,
                runtime_ref="runtime-a",
            )

        first = candidate("1.0.0", "a", "runtime-a")
        second = candidate("1.0.0", "a", "runtime-b")
        with self.assertRaisesRegex(CoordinatorError, "conflicting"):
            select_candidate(
                [claim(1, first), claim(2, second)],
                active_revision=None,
            )


class FakeOperations:
    def __init__(self, failures: set[str] | None = None) -> None:
        self.failures = failures or set()
        self.activations: list[str] = []
        self.restores = 0

    def activate(self, runtime: RuntimeCandidate) -> None:
        self.activations.append(runtime.content_revision)
        if runtime.content_revision in self.failures:
            raise RuntimeError("test activation failure")

    def restore_system(self) -> None:
        self.restores += 1


class VerifyingOperations(FakeOperations):
    def __init__(self) -> None:
        super().__init__()
        self.verifications: list[str] = []

    def verify_candidate(self, runtime: RuntimeCandidate) -> None:
        self.verifications.append(runtime.content_revision)


class RefreshingOperations(FakeOperations):
    def __init__(self) -> None:
        super().__init__()
        self.refreshed: list[tuple[str, str]] = []

    def refresh_generation(self, runtime: RuntimeCandidate) -> None:
        self.refreshed.append((runtime.content_revision, "refreshed"))


class LegacyOperations(FakeOperations):
    def __init__(self, legacy_revision: str) -> None:
        super().__init__()
        self.legacy_revision = legacy_revision

    def current_active_revision(self) -> str:
        return self.legacy_revision


class FlakyVerifyingOperations(VerifyingOperations):
    def __init__(self) -> None:
        super().__init__()
        self.fail_verification = True

    def verify_candidate(self, runtime: RuntimeCandidate) -> None:
        super().verify_candidate(runtime)
        if self.fail_verification:
            raise RuntimeError("test verification failure")


class RuntimeCoordinatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.paths = CoordinatorPaths.for_home(Path(self.directory.name))
        self.operations = FakeOperations()
        self.coordinator = RuntimeCoordinator(
            self.paths,
            self.operations,
            token_factory=lambda: "f" * 32,
        )

    def test_registration_persists_and_lower_claims_do_not_restart_the_active_core(self) -> None:
        low = candidate("1.0.0", "a")
        high = candidate("2.0.0", "b")

        first = self.coordinator.register(claim(1, low))
        second = self.coordinator.register(claim(2, low))
        third = self.coordinator.register(claim(3, high))
        fourth = self.coordinator.register(claim(4, low))

        self.assertEqual(first.action, "activated")
        self.assertEqual(second.action, "unchanged")
        self.assertEqual(third.action, "activated")
        self.assertEqual(fourth.action, "unchanged")
        self.assertEqual(
            self.operations.activations,
            [low.content_revision, high.content_revision],
        )

        restarted = RuntimeCoordinator(self.paths, self.operations)
        status = restarted.status()
        self.assertEqual(status.active_revision, high.content_revision)
        self.assertEqual(len(status.claims), 4)

    def test_same_product_reload_refreshes_generation_without_reactivation(self) -> None:
        operations = RefreshingOperations()
        coordinator = RuntimeCoordinator(self.paths, operations)
        runtime = candidate("1.0.0", "a")

        first = coordinator.register(
            RuntimeClaim("product.1", "generation-00000001", runtime)
        )
        reloaded = coordinator.register(
            RuntimeClaim("product.1", "generation-00000002", runtime)
        )

        self.assertEqual(first.action, "activated")
        self.assertEqual(reloaded.action, "unchanged")
        self.assertEqual(operations.activations, [runtime.content_revision])
        self.assertEqual(
            operations.refreshed,
            [(runtime.content_revision, "refreshed")],
        )
        self.assertEqual(
            coordinator.status().claims[0].generation,
            "generation-00000002",
        )

    def test_second_product_with_identical_content_does_not_refresh_owner(self) -> None:
        operations = RefreshingOperations()
        coordinator = RuntimeCoordinator(self.paths, operations)
        runtime = candidate("1.0.0", "a")

        coordinator.register(
            RuntimeClaim("product.1", "generation-00000001", runtime)
        )
        coordinator.register(
            RuntimeClaim("product.2", "generation-00000002", runtime)
        )

        self.assertEqual(operations.refreshed, [])

    def test_legacy_active_pointer_survives_a_failed_first_coordinator_activation(self) -> None:
        legacy = candidate("1.0.0", "a")
        replacement = candidate("2.0.0", "b")
        operations = LegacyOperations(legacy.content_revision)
        operations.failures.add(replacement.content_revision)
        coordinator = RuntimeCoordinator(self.paths, operations)

        failed = coordinator.register(
            RuntimeClaim("product.1", "generation-00000001", replacement)
        )

        self.assertEqual(failed.action, "failed")
        self.assertEqual(failed.active_revision, legacy.content_revision)
        self.assertEqual(operations.restores, 0)
        self.assertEqual(
            coordinator.status().active_revision,
            legacy.content_revision,
        )

        operations.failures.clear()
        retried = coordinator.retry(replacement.content_revision)
        self.assertEqual(retried.action, "activated")
        self.assertEqual(
            coordinator.status().active_revision,
            replacement.content_revision,
        )

    def test_existing_empty_coordinator_state_does_not_rebootstrap_legacy_state(self) -> None:
        original = candidate("1.0.0", "a")
        self.coordinator.register(claim(1, original))
        token = self.coordinator.mark_pending_removal(
            "product.1", "generation-00000001"
        )
        self.coordinator.finalize_removal(
            "product.1",
            "generation-00000001",
            token=token,
            plugin_present=False,
        )
        self.assertTrue(self.paths.state_file.exists())
        self.assertIsNone(self.coordinator.status().active_revision)

        replacement = candidate("2.0.0", "b")
        operations = LegacyOperations(original.content_revision)
        operations.failures.add(replacement.content_revision)
        coordinator = RuntimeCoordinator(self.paths, operations)

        failed = coordinator.register(
            RuntimeClaim("product.2", "generation-00000002", replacement)
        )

        self.assertEqual(failed.action, "failed")
        self.assertIsNone(failed.active_revision)
        self.assertIsNone(coordinator.status().active_revision)

    def test_unsupported_state_schema_is_read_only_failure(self) -> None:
        self.paths.root.mkdir(parents=True, mode=0o700)
        self.paths.state_file.write_text(
            json.dumps({"schema": 99, "claims": []}),
            encoding="utf-8",
        )
        self.paths.state_file.chmod(0o600)
        before = self.paths.state_file.read_bytes()

        with self.assertRaisesRegex(CoordinatorError, "unsupported") as raised:
            self.coordinator.status()

        self.assertEqual(raised.exception.code, "unsupported_state_schema")
        self.assertEqual(self.paths.state_file.read_bytes(), before)

    def test_pending_removal_does_not_stop_the_core_and_final_removal_restores_system(self) -> None:
        runtime = candidate("1.0.0", "a")
        self.coordinator.register(claim(1, runtime))
        token = self.coordinator.mark_pending_removal("product.1", "generation-00000001")

        waiting = self.coordinator.finalize_removal(
            "product.1",
            "generation-00000001",
            token=token,
            plugin_present=True,
        )
        finished = self.coordinator.finalize_removal(
            "product.1",
            "generation-00000001",
            token=token,
            plugin_present=False,
        )

        self.assertEqual(waiting.action, "pending")
        self.assertEqual(finished.action, "restored")
        self.assertEqual(self.operations.restores, 1)
        self.assertIsNone(self.coordinator.status().active_revision)

    def test_replacement_cancels_pending_removal(self) -> None:
        first = candidate("1.0.0", "a")
        replacement = candidate("2.0.0", "b")
        self.coordinator.register(claim(1, first))
        self.coordinator.mark_pending_removal("product.1", "generation-00000001")

        result = self.coordinator.register(
            RuntimeClaim("product.1", "generation-00000002", replacement)
        )
        with self.assertRaisesRegex(CoordinatorError, "stale"):
            self.coordinator.finalize_removal(
                "product.1",
                "generation-00000001",
                token="f" * 32,
                plugin_present=False,
            )

        self.assertEqual(result.action, "activated")
        status = self.coordinator.status()
        self.assertEqual(status.active_revision, replacement.content_revision)
        self.assertFalse(status.claims[0].pending_removal)

    def test_same_product_cannot_replace_a_claim_with_a_lower_core(self) -> None:
        high = candidate("2.0.0", "b")
        low = candidate("1.0.0", "a")
        self.coordinator.register(
            RuntimeClaim("product.1", "generation-00000001", high)
        )

        result = self.coordinator.register(
            RuntimeClaim("product.1", "generation-00000002", low)
        )

        self.assertEqual(result.action, "blocked")
        self.assertEqual(result.error, "lower_generation_cannot_downgrade")
        status = self.coordinator.status()
        self.assertEqual(status.claims[0].generation, "generation-00000001")
        self.assertEqual(status.active_revision, high.content_revision)

    def test_successful_verification_is_cached_by_content_revision(self) -> None:
        operations = VerifyingOperations()
        coordinator = RuntimeCoordinator(self.paths, operations)
        runtime = candidate("1.0.0", "a")

        first = coordinator.register(
            RuntimeClaim("product.1", "generation-00000001", runtime)
        )
        second = coordinator.register(
            RuntimeClaim("product.2", "generation-00000002", runtime)
        )

        self.assertEqual(first.action, "activated")
        self.assertEqual(second.action, "unchanged")
        self.assertEqual(operations.verifications, [runtime.content_revision])
        self.assertEqual(
            coordinator.status().verified_revisions, (runtime.content_revision,)
        )

    def test_failed_candidate_is_quarantined_until_explicit_retry(self) -> None:
        runtime = candidate("1.0.0", "a")
        self.operations.failures.add(runtime.content_revision)

        failed = self.coordinator.register(claim(1, runtime))
        repeated = self.coordinator.register(claim(2, runtime))
        self.operations.failures.clear()
        retried = self.coordinator.retry(runtime.content_revision)

        self.assertEqual(failed.action, "failed")
        self.assertEqual(repeated.action, "failed")
        self.assertEqual(retried.action, "activated")
        self.assertEqual(self.operations.activations.count(runtime.content_revision), 2)

    def test_failed_verification_retry_rebuilds_cache_before_activation(self) -> None:
        operations = FlakyVerifyingOperations()
        coordinator = RuntimeCoordinator(self.paths, operations)
        runtime = candidate("1.0.0", "a")

        failed = coordinator.register(
            RuntimeClaim("product.1", "generation-00000001", runtime)
        )
        self.assertEqual(failed.action, "failed")
        self.assertEqual(operations.verifications, [runtime.content_revision])
        self.assertEqual(operations.activations, [])
        self.assertEqual(coordinator.status().verified_revisions, ())
        self.assertIn(runtime.content_revision, coordinator.status().failed_revisions)

        operations.fail_verification = False
        retried = coordinator.retry(runtime.content_revision)

        self.assertEqual(retried.action, "activated")
        self.assertEqual(
            operations.verifications,
            [runtime.content_revision, runtime.content_revision],
        )
        self.assertEqual(operations.activations, [runtime.content_revision])
        self.assertEqual(
            coordinator.status().verified_revisions,
            (runtime.content_revision,),
        )

    def test_activation_failure_keeps_the_previous_known_good_revision(self) -> None:
        low = candidate("1.0.0", "a")
        middle = candidate("2.0.0", "b")
        high = candidate("3.0.0", "c")
        self.coordinator.register(claim(1, low))
        self.coordinator.register(claim(2, middle))
        self.operations.failures.update(
            {middle.content_revision, high.content_revision}
        )

        result = self.coordinator.register(claim(3, high))

        self.assertEqual(result.action, "failed")
        self.assertEqual(result.active_revision, middle.content_revision)
        self.assertEqual(self.operations.activations[-1], high.content_revision)


if __name__ == "__main__":
    unittest.main()
