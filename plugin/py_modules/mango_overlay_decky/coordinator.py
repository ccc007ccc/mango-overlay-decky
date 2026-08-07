"""Small, deterministic seam for shared Mango runtime arbitration.

The coordinator deliberately knows only bounded candidate metadata.  It does
not inspect or execute a runtime while comparing claims.  A lifecycle adapter
can use :func:`select_candidate` and verify the one returned candidate before
activating it.
"""

from __future__ import annotations

import fcntl
import json
import os
import re
import secrets
import stat
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Literal, Protocol


COORDINATOR_SCHEMA = 1
MAX_CLAIMS = 256
MAX_FAILED_REVISIONS = 256
MAX_VERIFIED_REVISIONS = 256
MAX_PRODUCT_ID_BYTES = 128
MAX_GENERATION_BYTES = 128
MAX_RUNTIME_REF_BYTES = 512
_MAX_VERSION_PART = 2**31 - 1
_CONTENT_REVISION = re.compile(r"[a-f0-9]{64}")
_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}")
_GENERATION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{7,127}")
_CORE_VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
_RUNTIME_REF = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,511}")
_TOKEN = re.compile(r"[a-f0-9]{32,64}")


class CoordinatorError(ValueError):
    """Stable validation or arbitration failure exposed at the seam."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, order=True)
class CoreVersion:
    """Comparable Mango semantic version used for candidate selection."""

    major: int
    minor: int
    patch: int

    @classmethod
    def parse(cls, value: str) -> "CoreVersion":
        if not isinstance(value, str) or _CORE_VERSION.fullmatch(value) is None:
            raise CoordinatorError("invalid_core_version", "Core version is not X.Y.Z")
        parts = tuple(int(part) for part in value.split("."))
        if any(part > _MAX_VERSION_PART for part in parts):
            raise CoordinatorError("invalid_core_version", "Core version part is too large")
        return cls(*parts)

    def as_string(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


@dataclass(frozen=True)
class RuntimeCandidate:
    """Bounded metadata for one immutable runtime content revision."""

    core_version: CoreVersion
    content_revision: str
    runtime_ref: str
    coordinator_schema: int = COORDINATOR_SCHEMA
    provider_protocol_min: int = 1
    provider_protocol_max: int = 1

    def __post_init__(self) -> None:
        if not isinstance(self.core_version, CoreVersion):
            raise CoordinatorError("invalid_core_version", "Candidate version is invalid")
        if _CONTENT_REVISION.fullmatch(self.content_revision) is None:
            raise CoordinatorError(
                "invalid_content_revision", "Candidate content revision is invalid"
            )
        if (
            not isinstance(self.runtime_ref, str)
            or _RUNTIME_REF.fullmatch(self.runtime_ref) is None
            or len(self.runtime_ref.encode("utf-8")) > MAX_RUNTIME_REF_BYTES
        ):
            raise CoordinatorError("invalid_runtime_ref", "Candidate runtime reference is invalid")
        if (
            type(self.coordinator_schema) is not int
            or self.coordinator_schema != COORDINATOR_SCHEMA
        ):
            raise CoordinatorError(
                "unsupported_coordinator_schema", "Candidate coordinator schema is unsupported"
            )
        if (
            type(self.provider_protocol_min) is not int
            or type(self.provider_protocol_max) is not int
            or self.provider_protocol_min < 0
            or self.provider_protocol_max < self.provider_protocol_min
            or self.provider_protocol_max > 2**16
        ):
            raise CoordinatorError(
                "invalid_provider_protocol", "Candidate provider protocol range is invalid"
            )

    @classmethod
    def create(
        cls,
        *,
        core_version: str,
        content_revision: str,
        runtime_ref: str,
        coordinator_schema: int = COORDINATOR_SCHEMA,
        provider_protocol_min: int = 1,
        provider_protocol_max: int = 1,
    ) -> "RuntimeCandidate":
        return cls(
            CoreVersion.parse(core_version),
            content_revision,
            runtime_ref,
            coordinator_schema,
            provider_protocol_min,
            provider_protocol_max,
        )


@dataclass(frozen=True)
class RuntimeClaim:
    """One product's durable demand for a candidate runtime."""

    product_id: str
    generation: str
    candidate: RuntimeCandidate
    pending_removal: bool = False
    pending_token: str | None = None

    def __post_init__(self) -> None:
        _validate_product_id(self.product_id)
        _validate_generation(self.generation)
        if not isinstance(self.candidate, RuntimeCandidate):
            raise CoordinatorError("invalid_candidate", "Runtime claim candidate is invalid")
        if type(self.pending_removal) is not bool:
            raise CoordinatorError("invalid_pending_removal", "Pending removal flag is invalid")
        if self.pending_token is not None and _TOKEN.fullmatch(self.pending_token) is None:
            raise CoordinatorError("invalid_pending_token", "Runtime claim pending token is invalid")
        if not self.pending_removal and self.pending_token is not None:
            raise CoordinatorError(
                "invalid_pending_token", "A non-pending claim cannot have a pending token"
            )


SelectionAction = Literal["unchanged", "activate", "restore", "blocked"]


@dataclass(frozen=True)
class CandidateSelection:
    """Pure result of comparing the bounded claim metadata."""

    action: SelectionAction
    candidate: RuntimeCandidate | None
    reason: str


@dataclass(frozen=True)
class CoordinatorPaths:
    """Private, product-independent storage owned by the coordinator."""

    root: Path
    state_file: Path
    lock_file: Path

    @classmethod
    def for_home(cls, home: Path) -> "CoordinatorPaths":
        root = home.absolute() / ".local/share/mango-overlay-decky/coordinator"
        return cls(root, root / "state.json", root / "coordinator.lock")


class RuntimeOperations(Protocol):
    """Adapter for the expensive, platform-specific activation side effects."""

    def ensure_candidate(self, candidate: RuntimeCandidate) -> None:
        """Cheap structural check that the retained candidate is available."""

    def verify_candidate(self, candidate: RuntimeCandidate) -> None:
        """Run the expensive self-test for the one selected candidate."""

    def activate(self, candidate: RuntimeCandidate) -> None:
        """Verify and make exactly this candidate the sole active runtime."""

    def restore_system(self) -> None:
        """Restore the system MangoApp after the last claim disappears."""

    def refresh_generation(self, candidate: RuntimeCandidate) -> None:
        """Refresh a replaced product claim without restarting the runtime."""

    def current_active_revision(self) -> str | None:
        """Return a legacy active revision when bootstrapping old installs."""

    def current_active_candidate(self) -> RuntimeCandidate | None:
        """Return bounded metadata for a legacy active revision, when known."""


@dataclass(frozen=True)
class CoordinatorStatus:
    active_revision: str | None
    known_good_revision: str | None
    claims: tuple[RuntimeClaim, ...]
    failed_revisions: tuple[str, ...]
    last_error: str | None
    verified_revisions: tuple[str, ...] = ()


CoordinatorAction = Literal[
    "unchanged",
    "activated",
    "restored",
    "blocked",
    "failed",
    "pending",
]


@dataclass(frozen=True)
class CoordinatorOutcome:
    action: CoordinatorAction
    active_revision: str | None
    candidate: RuntimeCandidate | None
    error: str | None = None


def select_candidate(
    claims: Iterable[RuntimeClaim],
    *,
    active_revision: str | None,
    failed_revisions: Iterable[str] = (),
) -> CandidateSelection:
    """Choose the highest usable candidate without executing any candidate.

    Claims that are pending removal remain eligible until a separate, explicit
    removal confirmation deletes them.  Identical content revisions are
    deduplicated.  Different content revisions at the same highest core
    version are a conflict; the caller must keep its known-good active
    revision and report the conflict instead of making a random choice.
    """

    if active_revision is not None and _CONTENT_REVISION.fullmatch(active_revision) is None:
        raise CoordinatorError("invalid_active_revision", "Active revision is invalid")

    claim_list: list[RuntimeClaim] = []
    for claim in claims:
        claim_list.append(claim)
        if len(claim_list) > MAX_CLAIMS:
            raise CoordinatorError("claim_limit", "Too many runtime claims")

    failed: set[str] = set()
    for revision in failed_revisions:
        failed.add(revision)
        if len(failed) > MAX_FAILED_REVISIONS:
            raise CoordinatorError("failed_revision_limit", "Too many failed revisions")
    if any(_CONTENT_REVISION.fullmatch(value) is None for value in failed):
        raise CoordinatorError("invalid_failed_revision", "Failed revision is invalid")

    by_revision: dict[str, RuntimeCandidate] = {}
    for claim in claim_list:
        candidate = claim.candidate
        prior = by_revision.get(candidate.content_revision)
        if prior is not None and prior != candidate:
            raise CoordinatorError(
                "candidate_conflict",
                "One content revision has conflicting candidate metadata",
            )
        by_revision[candidate.content_revision] = candidate

    candidates = [
        candidate
        for candidate in by_revision.values()
        if candidate.content_revision not in failed
    ]
    if not candidates:
        return CandidateSelection("restore", None, "no_usable_candidate")

    highest_version = max(candidate.core_version for candidate in candidates)
    highest = [
        candidate
        for candidate in candidates
        if candidate.core_version == highest_version
    ]
    if len(highest) > 1:
        return CandidateSelection("blocked", None, "same_version_conflict")

    selected = highest[0]
    if selected.content_revision == active_revision:
        return CandidateSelection("unchanged", selected, "active_is_highest")
    return CandidateSelection("activate", selected, "higher_or_replacement_candidate")


@dataclass
class _CoordinatorState:
    claims: list[RuntimeClaim]
    active_revision: str | None
    known_good_revision: str | None
    failed_revisions: list[str]
    verified_revisions: list[str]
    last_error: str | None = None


class RuntimeCoordinator:
    """Persistent coordinator with a deliberately small product interface.

    The coordinator owns only bounded metadata and activation ordering.  The
    supplied :class:`RuntimeOperations` adapter owns copying/verifying files,
    restarting the single broker/MangoApp, and restoring the system binary.
    Registration and removal are serialized by one private lock.  A claim is
    never removed merely because its process exited; callers must explicitly
    mark it pending and later confirm that the plugin directory is absent.
    """

    def __init__(
        self,
        paths: CoordinatorPaths,
        operations: RuntimeOperations,
        *,
        token_factory: Callable[[], str] = lambda: secrets.token_hex(16),
    ) -> None:
        self.paths = paths
        self._operations = operations
        self._token_factory = token_factory
        self._uid = os.geteuid()

    def status(self) -> CoordinatorStatus:
        with self._locked():
            state = self._read_state()
        return CoordinatorStatus(
            state.active_revision,
            state.known_good_revision,
            tuple(state.claims),
            tuple(state.failed_revisions),
            state.last_error,
            tuple(state.verified_revisions),
        )

    def register(self, claim: RuntimeClaim) -> CoordinatorOutcome:
        """Insert or replace one product claim and reconcile the active core."""

        with self._locked():
            try:
                self.paths.state_file.lstat()
            except FileNotFoundError:
                state_file_present = False
            else:
                state_file_present = True
            state = self._read_state()
            bootstrapped = False
            legacy_candidate: RuntimeCandidate | None = None
            if (
                not state_file_present
                and not state.claims
                and state.active_revision is None
            ):
                current_active_candidate = getattr(
                    self._operations, "current_active_candidate", None
                )
                if callable(current_active_candidate):
                    try:
                        candidate_value = current_active_candidate()
                    except Exception:
                        candidate_value = None
                    if isinstance(candidate_value, RuntimeCandidate):
                        legacy_candidate = candidate_value
                        state.active_revision = candidate_value.content_revision
                        state.known_good_revision = candidate_value.content_revision
                        bootstrapped = True
                current_active_revision = getattr(
                    self._operations, "current_active_revision", None
                )
                if not bootstrapped and callable(current_active_revision):
                    try:
                        legacy_revision = current_active_revision()
                    except Exception:
                        legacy_revision = None
                    if (
                        isinstance(legacy_revision, str)
                        and _CONTENT_REVISION.fullmatch(legacy_revision) is not None
                    ):
                        state.active_revision = legacy_revision
                        state.known_good_revision = legacy_revision
                        bootstrapped = True
            existing = next(
                (item for item in state.claims if item.product_id == claim.product_id),
                None,
            )
            if existing is not None and claim.candidate.core_version < existing.candidate.core_version:
                state.last_error = "lower_generation_cannot_downgrade"
                self._write_state(state)
                return CoordinatorOutcome(
                    "blocked",
                    state.active_revision,
                    existing.candidate,
                    state.last_error,
                )
            if (
                legacy_candidate is not None
                and claim.candidate.core_version < legacy_candidate.core_version
            ):
                state.last_error = "lower_generation_cannot_downgrade"
                # Leave the coordinator file absent during a rejected
                # first-register migration.  The next attempt must be able
                # to inspect the legacy candidate again rather than losing
                # its core version metadata.
                return CoordinatorOutcome(
                    "blocked",
                    state.active_revision,
                    legacy_candidate,
                    state.last_error,
                )
            ensure_candidate = getattr(self._operations, "ensure_candidate", None)
            if callable(ensure_candidate):
                try:
                    ensure_candidate(claim.candidate)
                except Exception as error:
                    state.last_error = "candidate_unavailable"
                    self._write_state(state)
                    return CoordinatorOutcome(
                        "failed", state.active_revision, claim.candidate, state.last_error
                    )
            state.claims = [
                existing
                for existing in state.claims
                if existing.product_id != claim.product_id
            ]
            state.claims.append(
                RuntimeClaim(
                    claim.product_id,
                    claim.generation,
                    claim.candidate,
                    False,
                    None,
                )
            )
            self._write_state(state)
            outcome = self._reconcile(state)
            # A same-product reload can keep the exact active content revision
            # and therefore take the unchanged fast path.  Its generation is
            # still the ownership token used by the uninstall callback.  Only
            # refresh when replacing an existing claim; a second product that
            # happens to ship identical bytes must not steal lifecycle
            # ownership from the product that activated the shared runtime.
            refresh_generation = getattr(self._operations, "refresh_generation", None)
            same_existing_revision = (
                existing is not None
                and existing.candidate.content_revision
                == claim.candidate.content_revision
            )
            same_bootstrapped_revision = (
                bootstrapped
                and outcome.active_revision == claim.candidate.content_revision
            )
            if (
                (same_existing_revision or same_bootstrapped_revision)
                and outcome.action == "unchanged"
                and callable(refresh_generation)
            ):
                try:
                    refresh_generation(claim.candidate)
                except Exception:
                    state.last_error = "generation_refresh_failed"
                    self._write_state(state)
                    return CoordinatorOutcome(
                        "failed",
                        state.active_revision,
                        claim.candidate,
                        state.last_error,
                    )
            return outcome

    def mark_pending_removal(self, product_id: str, generation: str) -> str:
        """Mark a claim pending; it remains eligible until final confirmation."""

        _validate_claim_identity(product_id, generation)
        with self._locked():
            state = self._read_state()
            index = _claim_index(state.claims, product_id, generation)
            token = self._token_factory()
            if _TOKEN.fullmatch(token) is None:
                raise CoordinatorError("invalid_pending_token", "Pending token is invalid")
            claim = state.claims[index]
            state.claims[index] = RuntimeClaim(
                claim.product_id,
                claim.generation,
                claim.candidate,
                True,
                token,
            )
            self._write_state(state)
            return token

    def finalize_removal(
        self,
        product_id: str,
        generation: str,
        *,
        token: str | None = None,
        plugin_present: bool,
    ) -> CoordinatorOutcome:
        """Remove a claim only after the caller proves the plugin is absent."""

        _validate_claim_identity(product_id, generation)
        if token is not None and _TOKEN.fullmatch(token) is None:
            raise CoordinatorError("invalid_pending_token", "Pending token is invalid")
        with self._locked():
            state = self._read_state()
            index = _claim_index(state.claims, product_id, generation, missing_ok=True)
            if index is None:
                return CoordinatorOutcome(
                    "unchanged", state.active_revision, None, "claim_not_found"
                )
            claim = state.claims[index]
            if not claim.pending_removal:
                return CoordinatorOutcome(
                    "pending", state.active_revision, claim.candidate, "removal_not_armed"
                )
            if token is not None and claim.pending_token != token:
                return CoordinatorOutcome(
                    "pending", state.active_revision, claim.candidate, "stale_pending_token"
                )
            if plugin_present:
                return CoordinatorOutcome(
                    "pending", state.active_revision, claim.candidate, "plugin_still_present"
                )
            state.claims.pop(index)
            self._write_state(state)
            return self._reconcile(state)

    def retry(self, content_revision: str | None = None) -> CoordinatorOutcome:
        """Explicitly clear failed revisions and retry normal reconciliation."""

        if content_revision is not None and _CONTENT_REVISION.fullmatch(content_revision) is None:
            raise CoordinatorError("invalid_content_revision", "Failed revision is invalid")
        with self._locked():
            state = self._read_state()
            if content_revision is None:
                failed = set(state.failed_revisions)
                state.failed_revisions.clear()
                state.verified_revisions = [
                    revision
                    for revision in state.verified_revisions
                    if revision not in failed
                ]
            else:
                state.failed_revisions = [
                    revision
                    for revision in state.failed_revisions
                    if revision != content_revision
                ]
                state.verified_revisions = [
                    revision
                    for revision in state.verified_revisions
                    if revision != content_revision
                ]
            self._write_state(state)
            return self._reconcile(state)

    def _reconcile(self, state: _CoordinatorState) -> CoordinatorOutcome:
        selection = select_candidate(
            state.claims,
            active_revision=state.active_revision,
            failed_revisions=state.failed_revisions,
        )
        if selection.action == "unchanged":
            state.last_error = None
            self._write_state(state)
            return CoordinatorOutcome("unchanged", state.active_revision, selection.candidate)
        if selection.action == "blocked":
            state.last_error = "same_version_conflict"
            self._write_state(state)
            return CoordinatorOutcome("blocked", state.active_revision, None, state.last_error)
        if selection.action == "restore":
            if state.claims:
                # A failed candidate must not tear down an already active
                # known-good runtime.  Keep its pointer until an explicit
                # removal or a successful retry changes it.
                state.last_error = "no_usable_candidate"
                self._write_state(state)
                return CoordinatorOutcome(
                    "failed", state.active_revision, None, state.last_error
                )
            if state.active_revision is None:
                state.last_error = None
                self._write_state(state)
                return CoordinatorOutcome("unchanged", None, None)
            try:
                self._operations.restore_system()
            except Exception:
                state.last_error = "system_restore_failed"
                self._write_state(state)
                return CoordinatorOutcome("failed", state.active_revision, None, state.last_error)
            state.active_revision = None
            state.known_good_revision = None
            state.last_error = None
            self._write_state(state)
            return CoordinatorOutcome("restored", None, None)

        candidate = selection.candidate
        assert candidate is not None
        return self._activate_or_fallback(state, candidate)

    def _activate_or_fallback(
        self,
        state: _CoordinatorState,
        candidate: RuntimeCandidate,
        *,
        after_failure: bool = False,
    ) -> CoordinatorOutcome:
        """Try only the selected revision, then descend by version on failure."""

        was_verified = candidate.content_revision in state.verified_revisions
        verify_candidate = getattr(self._operations, "verify_candidate", None)
        try:
            if not was_verified and callable(verify_candidate):
                verify_candidate(candidate)
            self._operations.activate(candidate)
            if not was_verified:
                state.verified_revisions.append(candidate.content_revision)
                state.verified_revisions = list(
                    dict.fromkeys(state.verified_revisions)
                )[-MAX_VERIFIED_REVISIONS:]
        except Exception:
            if candidate.content_revision in state.verified_revisions:
                state.verified_revisions = [
                    revision
                    for revision in state.verified_revisions
                    if revision != candidate.content_revision
                ]
            if candidate.content_revision not in state.failed_revisions:
                state.failed_revisions.append(candidate.content_revision)
                state.failed_revisions = state.failed_revisions[-MAX_FAILED_REVISIONS:]
            state.last_error = "candidate_activation_failed"
            self._write_state(state)
            fallback = select_candidate(
                state.claims,
                active_revision=state.active_revision,
                failed_revisions=state.failed_revisions,
            )
            if fallback.action == "activate" and fallback.candidate is not None:
                return self._activate_or_fallback(
                    state, fallback.candidate, after_failure=True
                )
            if fallback.action == "blocked":
                state.last_error = "same_version_conflict"
                self._write_state(state)
                return CoordinatorOutcome("blocked", state.active_revision, None, state.last_error)
            if state.active_revision is not None:
                self._write_state(state)
                return CoordinatorOutcome(
                    "failed", state.active_revision, None, state.last_error
                )
            try:
                self._operations.restore_system()
            except Exception:
                state.last_error = "system_restore_failed"
                self._write_state(state)
                return CoordinatorOutcome("failed", None, None, state.last_error)
            state.known_good_revision = None
            self._write_state(state)
            return CoordinatorOutcome("failed", None, None, state.last_error)

        state.active_revision = candidate.content_revision
        state.known_good_revision = candidate.content_revision
        state.last_error = (
            "candidate_activation_failed_fallback" if after_failure else None
        )
        self._write_state(state)
        return CoordinatorOutcome(
            "activated", state.active_revision, candidate, state.last_error
        )

    @contextmanager
    def _locked(self):
        try:
            info = self.paths.root.lstat()
        except FileNotFoundError:
            self.paths.root.mkdir(parents=True, mode=0o700)
            info = self.paths.root.lstat()
        if (
            not stat.S_ISDIR(info.st_mode)
            or info.st_uid != self._uid
            or info.st_mode & 0o022
        ):
            raise CoordinatorError("unsafe_root", "Coordinator storage is not private")
        os.chmod(self.paths.root, 0o700)
        descriptor = os.open(
            self.paths.lock_file,
            os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            info = os.fstat(descriptor)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != self._uid
                or info.st_mode & 0o022
            ):
                raise CoordinatorError("unsafe_lock", "Coordinator lock is not private")
            os.fchmod(descriptor, 0o600)
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            yield
        finally:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)

    def _read_state(self) -> _CoordinatorState:
        try:
            info = self.paths.state_file.lstat()
        except FileNotFoundError:
            return _CoordinatorState([], None, None, [], [])
        except OSError as error:
            raise CoordinatorError("state_read_failed", "Coordinator state cannot be read") from error
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != self._uid
            or info.st_mode & 0o022
            or info.st_size > 512 * 1024
        ):
            raise CoordinatorError("unsafe_state", "Coordinator state is not private")
        try:
            raw = json.loads(self.paths.state_file.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise CoordinatorError("invalid_state", "Coordinator state is invalid") from error
        if not isinstance(raw, dict) or raw.get("schema") != COORDINATOR_SCHEMA:
            raise CoordinatorError("unsupported_state_schema", "Coordinator state schema is unsupported")
        raw_claims = raw.get("claims", [])
        raw_failed = raw.get("failed_revisions", [])
        raw_verified = raw.get("verified_revisions", [])
        if (
            not isinstance(raw_claims, list)
            or not isinstance(raw_failed, list)
            or not isinstance(raw_verified, list)
        ):
            raise CoordinatorError("invalid_state", "Coordinator state collections are invalid")
        if (
            len(raw_claims) > MAX_CLAIMS
            or len(raw_failed) > MAX_FAILED_REVISIONS
            or len(raw_verified) > MAX_VERIFIED_REVISIONS
        ):
            raise CoordinatorError("state_limit", "Coordinator state is too large")
        claims: list[RuntimeClaim] = []
        product_ids: set[str] = set()
        for raw_claim in raw_claims:
            claim = _claim_from_json(raw_claim)
            if claim.product_id in product_ids:
                raise CoordinatorError("invalid_state", "Coordinator state has duplicate claims")
            product_ids.add(claim.product_id)
            claims.append(claim)
        failed = []
        for revision in raw_failed:
            if not isinstance(revision, str) or _CONTENT_REVISION.fullmatch(revision) is None:
                raise CoordinatorError("invalid_state", "Coordinator state has an invalid failed revision")
            if revision not in failed:
                failed.append(revision)
        verified = []
        for revision in raw_verified:
            if not isinstance(revision, str) or _CONTENT_REVISION.fullmatch(revision) is None:
                raise CoordinatorError("invalid_state", "Coordinator state has an invalid verified revision")
            if revision not in verified:
                verified.append(revision)
        active = raw.get("active_revision")
        known_good = raw.get("known_good_revision")
        for value in (active, known_good):
            if value is not None and (
                not isinstance(value, str) or _CONTENT_REVISION.fullmatch(value) is None
            ):
                raise CoordinatorError("invalid_state", "Coordinator state has an invalid active revision")
        last_error = raw.get("last_error")
        if last_error is not None and (
            not isinstance(last_error, str) or len(last_error.encode("utf-8")) > 256
        ):
            raise CoordinatorError("invalid_state", "Coordinator state has an invalid error")
        return _CoordinatorState(claims, active, known_good, failed, verified, last_error)

    def _write_state(self, state: _CoordinatorState) -> None:
        value = {
            "schema": COORDINATOR_SCHEMA,
            "active_revision": state.active_revision,
            "known_good_revision": state.known_good_revision,
            "failed_revisions": sorted(set(state.failed_revisions)),
            "verified_revisions": sorted(set(state.verified_revisions)),
            "last_error": state.last_error,
            "claims": [_claim_to_json(claim) for claim in state.claims],
        }
        encoded = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".state.", suffix=".tmp", dir=self.paths.root
        )
        temporary = Path(temporary_name)
        try:
            os.fchmod(descriptor, 0o600)
            with os.fdopen(descriptor, "wb") as destination:
                destination.write(encoded)
                destination.flush()
                os.fsync(destination.fileno())
            os.replace(temporary, self.paths.state_file)
            directory = os.open(self.paths.root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        except OSError as error:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
            raise CoordinatorError("state_write_failed", "Coordinator state cannot be written") from error


def _validate_claim_identity(product_id: str, generation: str) -> None:
    _validate_product_id(product_id)
    _validate_generation(generation)


def _validate_product_id(product_id: str) -> None:
    if (
        not isinstance(product_id, str)
        or _IDENTIFIER.fullmatch(product_id) is None
        or len(product_id.encode("utf-8")) > MAX_PRODUCT_ID_BYTES
    ):
        raise CoordinatorError("invalid_product_id", "Runtime claim product identity is invalid")


def _validate_generation(generation: str) -> None:
    if (
        not isinstance(generation, str)
        or _GENERATION.fullmatch(generation) is None
        or len(generation.encode("utf-8")) > MAX_GENERATION_BYTES
    ):
        raise CoordinatorError("invalid_generation", "Runtime claim generation is invalid")


def _claim_index(
    claims: list[RuntimeClaim],
    product_id: str,
    generation: str,
    *,
    missing_ok: bool = False,
) -> int | None:
    for index, claim in enumerate(claims):
        if claim.product_id == product_id:
            if claim.generation != generation:
                raise CoordinatorError("stale_generation", "Claim generation is stale")
            return index
    if missing_ok:
        return None
    raise CoordinatorError("claim_not_found", "Runtime claim does not exist")


def _candidate_to_json(candidate: RuntimeCandidate) -> dict[str, object]:
    return {
        "core_version": candidate.core_version.as_string(),
        "content_revision": candidate.content_revision,
        "runtime_ref": candidate.runtime_ref,
        "coordinator_schema": candidate.coordinator_schema,
        "provider_protocol_min": candidate.provider_protocol_min,
        "provider_protocol_max": candidate.provider_protocol_max,
    }


def _candidate_from_json(raw: object) -> RuntimeCandidate:
    if not isinstance(raw, dict):
        raise CoordinatorError("invalid_state", "Candidate metadata is invalid")
    try:
        return RuntimeCandidate.create(
            core_version=raw["core_version"],  # type: ignore[arg-type]
            content_revision=raw["content_revision"],  # type: ignore[arg-type]
            runtime_ref=raw["runtime_ref"],  # type: ignore[arg-type]
            coordinator_schema=raw.get("coordinator_schema", COORDINATOR_SCHEMA),  # type: ignore[arg-type]
            provider_protocol_min=raw.get("provider_protocol_min", 1),  # type: ignore[arg-type]
            provider_protocol_max=raw.get("provider_protocol_max", 1),  # type: ignore[arg-type]
        )
    except (KeyError, TypeError, CoordinatorError) as error:
        if isinstance(error, CoordinatorError):
            raise CoordinatorError("invalid_state", "Candidate metadata is invalid") from error
        raise CoordinatorError("invalid_state", "Candidate metadata is invalid") from error


def _claim_to_json(claim: RuntimeClaim) -> dict[str, object]:
    return {
        "product_id": claim.product_id,
        "generation": claim.generation,
        "pending_removal": claim.pending_removal,
        "pending_token": claim.pending_token,
        "candidate": _candidate_to_json(claim.candidate),
    }


def _claim_from_json(raw: object) -> RuntimeClaim:
    if not isinstance(raw, dict):
        raise CoordinatorError("invalid_state", "Claim metadata is invalid")
    try:
        return RuntimeClaim(
            raw["product_id"],  # type: ignore[arg-type]
            raw["generation"],  # type: ignore[arg-type]
            _candidate_from_json(raw["candidate"]),
            raw.get("pending_removal", False),  # type: ignore[arg-type]
            raw.get("pending_token"),  # type: ignore[arg-type]
        )
    except (KeyError, TypeError, CoordinatorError) as error:
        if isinstance(error, CoordinatorError):
            raise CoordinatorError("invalid_state", "Claim metadata is invalid") from error
        raise CoordinatorError("invalid_state", "Claim metadata is invalid") from error
