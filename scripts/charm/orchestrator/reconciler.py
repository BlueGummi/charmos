"""Pure desired/actual-state transitions for durable Nightmare orchestration."""

from dataclasses import dataclass, replace
from datetime import datetime, timedelta
from typing import Any

from ..nightmare import contracts
from . import state as S


@dataclass(frozen=True)
class SubmitCommand:
    document: dict[str, Any]
    request_id: str | None = None
    wake: bool = True


@dataclass(frozen=True)
class AcquireCoordinator:
    wake_id: str
    owner: str
    lease_ms: int


@dataclass(frozen=True)
class RenewCoordinator:
    owner: str
    lease_ms: int


@dataclass(frozen=True)
class RecordPlan:
    command_id: str
    plan: dict[str, Any]
    snapshot: dict[str, Any] | None
    expected_authority_revision: int | None = None
    stored_plan: dict[str, Any] | None = None
    related_plans: dict[str, dict[str, Any]] | None = None


@dataclass(frozen=True)
class RecordSnapshot:
    snapshot: dict[str, Any]
    expected_authority_revision: int


@dataclass(frozen=True)
class CommitBatch:
    command_id: str
    plan_id: str
    batch: dict[str, Any]
    snapshot: dict[str, Any]
    result: dict[str, Any]
    expected_authority_revision: int
    expected_batch_version: str | None = None
    wake: bool = False


@dataclass(frozen=True)
class FinalizeBatch:
    attempt_id: str
    batch: dict[str, Any]
    snapshot: dict[str, Any]
    expected_authority_revision: int
    expected_batch_version: str
    partial: bool


@dataclass(frozen=True)
class StartExecution:
    command_id: str
    owner: str


@dataclass(frozen=True)
class RegisterAttempt:
    attempt_id: str
    plan_id: str
    manifest_ids: tuple[str, ...]


@dataclass(frozen=True)
class RecordResult:
    attempt_id: str
    manifest_id: str
    sha256: str


@dataclass(frozen=True)
class ArtifactLost:
    attempt_id: str
    manifest_id: str


@dataclass(frozen=True)
class CompleteDrain:
    owner: str


@dataclass(frozen=True)
class Tick:
    idle_grace_ms: int


@dataclass(frozen=True)
class MarkDispatched:
    outbox_id: str
    external_id: str


Event = (
    SubmitCommand
    | AcquireCoordinator
    | RenewCoordinator
    | RecordPlan
    | RecordSnapshot
    | CommitBatch
    | FinalizeBatch
    | StartExecution
    | RegisterAttempt
    | RecordResult
    | ArtifactLost
    | CompleteDrain
    | Tick
    | MarkDispatched
)


@dataclass(frozen=True)
class Transition:
    state: S.OrchestratorState
    result: dict[str, Any]
    changed: bool = True


def _wake(
    state: S.OrchestratorState,
    *,
    generation: int,
    reason: str,
    command_ref: str,
    now: datetime,
) -> tuple[S.OrchestratorState, S.OutboxEntry]:
    wake_digest = contracts.sha256_json(
        {
            "generation": generation,
            "reason": reason,
            "command_ref": command_ref,
        }
    )
    wake_id = f"wake_{generation}_{wake_digest[:16]}"
    existing = state.outbox.get(wake_id)
    if existing is not None:
        return state, existing
    entry = S.OutboxEntry(
        id=wake_id,
        generation=generation,
        reason=reason,
        command_ref=command_ref,
        created_at=S.instant(now),
    )
    outbox = {**state.outbox, wake_id: entry}
    return replace(state, outbox=outbox), entry


def _lease_active(lease: S.CoordinatorLease | None, now: datetime) -> bool:
    return lease is not None and S.parse_instant(lease.expires_at) > now


class Reconciler:
    """One event and the authoritative clock produce one deterministic transition."""

    def evolve(
        self, state: S.OrchestratorState, event: Event, *, now: datetime
    ) -> Transition:
        if now.tzinfo is None:
            raise ValueError("reconciler clock must include a UTC offset")
        if isinstance(event, SubmitCommand):
            return self._submit(state, event, now)
        if isinstance(event, AcquireCoordinator):
            return self._acquire(state, event, now)
        if isinstance(event, RenewCoordinator):
            return self._renew(state, event, now)
        if isinstance(event, RecordPlan):
            return self._record_plan(state, event)
        if isinstance(event, RecordSnapshot):
            return self._record_snapshot(state, event)
        if isinstance(event, CommitBatch):
            return self._commit_batch(state, event, now)
        if isinstance(event, FinalizeBatch):
            return self._finalize_batch(state, event)
        if isinstance(event, StartExecution):
            return self._start_execution(state, event, now)
        if isinstance(event, RegisterAttempt):
            return self._register_attempt(state, event)
        if isinstance(event, RecordResult):
            return self._record_result(state, event)
        if isinstance(event, ArtifactLost):
            return self._artifact_lost(state, event, now)
        if isinstance(event, CompleteDrain):
            return self._complete_drain(state, event, now)
        if isinstance(event, Tick):
            return self._tick(state, event, now)
        return self._mark_dispatched(state, event, now)

    def _submit(
        self, state: S.OrchestratorState, event: SubmitCommand, now: datetime
    ) -> Transition:
        document = event.document
        try:
            command_id = document["command_id"]
            key = document["idempotency_key"]
        except (KeyError, TypeError) as error:
            return Transition(
                state,
                {"kind": "rejected", "code": "invalid_command", "message": str(error)},
                False,
            )
        digest = contracts.sha256_json(document)
        prior = state.idempotency.get(key)
        if prior is not None:
            if prior["sha256"] != digest:
                return Transition(
                    state,
                    {
                        "kind": "conflict",
                        "code": "idempotency_conflict",
                        "command_id": prior["command_id"],
                    },
                    False,
                )
            entry = state.commands[prior["command_id"]]
            return Transition(
                state,
                entry.result
                or {
                    "kind": "accepted",
                    "command_id": entry.command_id,
                    "status": entry.status,
                },
                False,
            )
        if command_id in state.commands:
            return Transition(
                state,
                {"kind": "conflict", "code": "command_id_conflict"},
                False,
            )
        coordinator = state.coordinator
        generation = (
            coordinator.generation + 1
            if coordinator.status
            in (S.CoordinatorStatus.SLEEPING, S.CoordinatorStatus.RUNNING)
            else coordinator.generation
        )
        command = S.CommandEntry(
            command_id=command_id,
            idempotency_key=key,
            sha256=digest,
            generation=generation,
            status=S.CommandStatus.QUEUED.value,
            received_at=S.instant(now),
            document=document,
            request_id=event.request_id,
        )
        next_state = replace(
            state,
            commands={**state.commands, command_id: command},
            idempotency={
                **state.idempotency,
                key: {"command_id": command_id, "sha256": digest},
            },
        )
        if event.wake and coordinator.status == S.CoordinatorStatus.SLEEPING:
            next_state = replace(
                next_state,
                coordinator=S.CoordinatorState(
                    status=S.CoordinatorStatus.WAKING.value,
                    generation=generation,
                ),
            )
            next_state, wake = _wake(
                next_state,
                generation=generation,
                reason="commands_available",
                command_ref=command_id,
                now=now,
            )
            wake_id: str | None = wake.id
        else:
            wake_id = None
        return Transition(
            next_state,
            {
                "kind": "accepted",
                "command_id": command_id,
                "status": "queued",
                "generation": generation,
                "wake_id": wake_id,
            },
        )

    def _acquire(
        self, state: S.OrchestratorState, event: AcquireCoordinator, now: datetime
    ) -> Transition:
        coordinator = state.coordinator
        wake = state.outbox.get(event.wake_id)
        if wake is None or wake.generation != coordinator.generation:
            return Transition(
                state,
                {"kind": "rejected", "code": "unknown_wake"},
                False,
            )
        if _lease_active(coordinator.lease, now):
            lease = coordinator.lease
            if (
                lease is not None
                and lease.owner == event.owner
                and lease.wake_id == event.wake_id
            ):
                return Transition(state, {"kind": "accepted", "lease": "held"}, False)
            return Transition(
                state,
                {"kind": "conflict", "code": "coordinator_leased"},
                False,
            )
        if coordinator.status != S.CoordinatorStatus.WAKING:
            return Transition(
                state,
                {"kind": "rejected", "code": "wake_consumed"},
                False,
            )
        lease = S.CoordinatorLease(
            owner=event.owner,
            wake_id=event.wake_id,
            generation=wake.generation,
            expires_at=S.instant(now + timedelta(milliseconds=event.lease_ms)),
        )
        return Transition(
            replace(
                state,
                coordinator=S.CoordinatorState(
                    status=S.CoordinatorStatus.RUNNING.value,
                    generation=wake.generation,
                    lease=lease,
                ),
            ),
            {"kind": "accepted", "lease": "acquired", "generation": wake.generation},
        )

    def _renew(
        self, state: S.OrchestratorState, event: RenewCoordinator, now: datetime
    ) -> Transition:
        lease = state.coordinator.lease
        if lease is None or lease.owner != event.owner or not _lease_active(lease, now):
            return Transition(state, {"kind": "conflict", "code": "lease_lost"}, False)
        renewed = replace(
            lease, expires_at=S.instant(now + timedelta(milliseconds=event.lease_ms))
        )
        return Transition(
            replace(state, coordinator=replace(state.coordinator, lease=renewed)),
            {"kind": "accepted", "lease": "renewed"},
        )

    def _record_plan(self, state: S.OrchestratorState, event: RecordPlan) -> Transition:
        command = state.commands.get(event.command_id)
        if command is None:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_command"}, False
            )
        if (
            event.expected_authority_revision is not None
            and event.expected_authority_revision != state.authority_revision
        ):
            result = {
                "kind": "stale",
                "code": "snapshot_changed",
                "message": "Fleet state changed. Validate this definition again.",
                "currentVersion": event.snapshot["version"] if event.snapshot else "",
            }
            updated = replace(
                command, status=S.CommandStatus.REJECTED.value, result=result
            )
            return Transition(
                replace(
                    state, commands={**state.commands, command.command_id: updated}
                ),
                result,
            )
        plan_doc = (
            event.plan.get("plan") if event.plan.get("kind") == "accepted" else None
        )
        if not isinstance(plan_doc, dict):
            result = event.plan
            updated = replace(
                command, status=S.CommandStatus.REJECTED.value, result=result
            )
            return Transition(
                replace(
                    state, commands={**state.commands, command.command_id: updated}
                ),
                result,
            )
        plan_id = plan_doc["id"]
        snapshots = state.snapshots
        if event.snapshot is not None:
            snapshots = {**snapshots, event.snapshot["version"]: event.snapshot}
        result = event.plan
        updated = replace(command, status=S.CommandStatus.APPLIED.value, result=result)
        return Transition(
            replace(
                state,
                commands={**state.commands, command.command_id: updated},
                plans={
                    **state.plans,
                    plan_id: event.stored_plan or event.plan,
                    **(event.related_plans or {}),
                },
                snapshots=snapshots,
            ),
            result,
        )

    def _record_snapshot(
        self, state: S.OrchestratorState, event: RecordSnapshot
    ) -> Transition:
        if event.expected_authority_revision != state.authority_revision:
            return Transition(
                state, {"kind": "conflict", "code": "snapshot_changed"}, False
            )
        version = event.snapshot["version"]
        if state.snapshots.get(version) == event.snapshot:
            return Transition(state, {"kind": "accepted", "version": version}, False)
        return Transition(
            replace(state, snapshots={**state.snapshots, version: event.snapshot}),
            {"kind": "accepted", "version": version},
        )

    def _commit_batch(
        self, state: S.OrchestratorState, event: CommitBatch, now: datetime
    ) -> Transition:
        command = state.commands.get(event.command_id)
        if command is None:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_command"}, False
            )
        if event.expected_authority_revision != state.authority_revision:
            return Transition(
                state, {"kind": "conflict", "code": "snapshot_changed"}, False
            )
        batch_id = event.batch["id"]
        current = state.batches.get(batch_id)
        if event.expected_batch_version is None:
            if current is not None:
                return Transition(
                    state, {"kind": "conflict", "code": "batch_exists"}, False
                )
        elif current is None or current.get("version") != event.expected_batch_version:
            return Transition(
                state, {"kind": "conflict", "code": "batch_changed"}, False
            )
        command_generation = command.generation
        command_status = S.CommandStatus.APPLIED.value
        if event.wake:
            command_status = S.CommandStatus.QUEUED.value
            if state.coordinator.status in (
                S.CoordinatorStatus.WAKING,
                S.CoordinatorStatus.RUNNING,
            ):
                command_generation = max(
                    command_generation, state.coordinator.generation + 1
                )
        updated = replace(
            command,
            generation=command_generation,
            status=command_status,
            result=event.result,
        )
        next_state = replace(
            state,
            commands={**state.commands, command.command_id: updated},
            batches={**state.batches, batch_id: event.batch},
            batch_plans={**state.batch_plans, batch_id: event.plan_id},
            snapshots={
                **state.snapshots,
                event.snapshot["version"]: event.snapshot,
            },
            authority_revision=state.authority_revision + 1,
        )
        if event.wake and state.coordinator.status == S.CoordinatorStatus.SLEEPING:
            generation = command_generation
            next_state = replace(
                next_state,
                coordinator=S.CoordinatorState(
                    status=S.CoordinatorStatus.WAKING.value,
                    generation=generation,
                ),
            )
            next_state, _wake_entry = _wake(
                next_state,
                generation=generation,
                reason="commands_available",
                command_ref=command.command_id,
                now=now,
            )
        return Transition(next_state, event.result)

    def _start_execution(
        self, state: S.OrchestratorState, event: StartExecution, now: datetime
    ) -> Transition:
        command = state.commands.get(event.command_id)
        lease = state.coordinator.lease
        if command is None:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_command"}, False
            )
        if lease is None or lease.owner != event.owner or not _lease_active(lease, now):
            return Transition(state, {"kind": "conflict", "code": "lease_lost"}, False)
        if command.generation != lease.generation:
            return Transition(
                state, {"kind": "conflict", "code": "wrong_generation"}, False
            )
        if command.status == S.CommandStatus.APPLIED.value:
            return Transition(state, {"kind": "accepted", "status": "started"}, False)
        if command.status != S.CommandStatus.QUEUED.value:
            return Transition(
                state, {"kind": "rejected", "code": "command_not_executable"}, False
            )
        updated = replace(command, status=S.CommandStatus.APPLIED.value)
        return Transition(
            replace(state, commands={**state.commands, command.command_id: updated}),
            {"kind": "accepted", "status": "started"},
        )

    def _finalize_batch(
        self, state: S.OrchestratorState, event: FinalizeBatch
    ) -> Transition:
        attempt = state.attempts.get(event.attempt_id)
        if attempt is None:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_attempt"}, False
            )
        if event.expected_authority_revision != state.authority_revision:
            return Transition(
                state, {"kind": "conflict", "code": "snapshot_changed"}, False
            )
        batch_id = event.batch["id"]
        current = state.batches.get(batch_id)
        if current is None or current.get("version") != event.expected_batch_version:
            return Transition(
                state, {"kind": "conflict", "code": "batch_changed"}, False
            )
        status = (
            S.AttemptStatus.RECOVERY_REQUIRED.value
            if event.partial
            else S.AttemptStatus.COMPLETED.value
        )
        updated_attempt = replace(attempt, status=status)
        return Transition(
            replace(
                state,
                batches={**state.batches, batch_id: event.batch},
                snapshots={
                    **state.snapshots,
                    event.snapshot["version"]: event.snapshot,
                },
                attempts={**state.attempts, event.attempt_id: updated_attempt},
                authority_revision=state.authority_revision + 1,
            ),
            {
                "kind": "accepted",
                "batch_id": batch_id,
                "partial": event.partial,
            },
        )

    def _register_attempt(
        self, state: S.OrchestratorState, event: RegisterAttempt
    ) -> Transition:
        existing = state.attempts.get(event.attempt_id)
        if existing is not None:
            same = (
                existing.plan_id == event.plan_id
                and existing.manifest_ids == event.manifest_ids
            )
            return Transition(
                state,
                {"kind": "accepted" if same else "conflict", "code": "attempt_exists"},
                False,
            )
        if event.plan_id not in state.plans:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_plan"}, False
            )
        attempt = S.AttemptEntry(
            event.attempt_id,
            event.plan_id,
            state.coordinator.generation,
            tuple(sorted(event.manifest_ids)),
            status=S.AttemptStatus.RUNNING.value,
        )
        return Transition(
            replace(state, attempts={**state.attempts, event.attempt_id: attempt}),
            {"kind": "accepted", "attempt_id": event.attempt_id},
        )

    def _record_result(
        self, state: S.OrchestratorState, event: RecordResult
    ) -> Transition:
        attempt = state.attempts.get(event.attempt_id)
        if attempt is None or event.manifest_id not in attempt.manifest_ids:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_manifest"}, False
            )
        prior = attempt.result_digests.get(event.manifest_id)
        if prior is not None:
            if prior != event.sha256:
                return Transition(
                    state,
                    {"kind": "conflict", "code": "result_mismatch"},
                    False,
                )
            if event.manifest_id in attempt.missing_artifacts:
                missing = tuple(
                    item
                    for item in attempt.missing_artifacts
                    if item != event.manifest_id
                )
                complete = (
                    len(attempt.result_digests) == len(attempt.manifest_ids)
                    and not missing
                )
                updated = replace(
                    attempt,
                    missing_artifacts=missing,
                    status=(
                        S.AttemptStatus.COMPLETED.value
                        if complete
                        else S.AttemptStatus.RUNNING.value
                    ),
                )
                return Transition(
                    replace(
                        state,
                        attempts={**state.attempts, event.attempt_id: updated},
                    ),
                    {"kind": "accepted", "complete": complete, "recovered": True},
                )
            return Transition(
                state,
                {"kind": "accepted", "code": "duplicate_result"},
                False,
            )
        results = {**attempt.result_digests, event.manifest_id: event.sha256}
        missing = tuple(
            item for item in attempt.missing_artifacts if item != event.manifest_id
        )
        complete = len(results) == len(attempt.manifest_ids) and not missing
        updated = replace(
            attempt,
            result_digests=results,
            missing_artifacts=missing,
            status=(
                S.AttemptStatus.COMPLETED.value
                if complete
                else S.AttemptStatus.RUNNING.value
            ),
        )
        return Transition(
            replace(state, attempts={**state.attempts, event.attempt_id: updated}),
            {"kind": "accepted", "complete": complete},
        )

    def _artifact_lost(
        self, state: S.OrchestratorState, event: ArtifactLost, now: datetime
    ) -> Transition:
        attempt = state.attempts.get(event.attempt_id)
        if attempt is None or event.manifest_id not in attempt.manifest_ids:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_manifest"}, False
            )
        missing = tuple(sorted(set(attempt.missing_artifacts) | {event.manifest_id}))
        updated = replace(
            attempt,
            missing_artifacts=missing,
            status=S.AttemptStatus.RECOVERY_REQUIRED.value,
        )
        generation = max(state.coordinator.generation + 1, attempt.generation + 1)
        next_state = replace(
            state,
            attempts={**state.attempts, event.attempt_id: updated},
            coordinator=S.CoordinatorState(
                status=S.CoordinatorStatus.WAKING.value, generation=generation
            ),
        )
        next_state, wake = _wake(
            next_state,
            generation=generation,
            reason="recovery",
            command_ref=event.attempt_id,
            now=now,
        )
        return Transition(
            next_state,
            {"kind": "accepted", "recovery_required": True, "wake_id": wake.id},
        )

    def _complete_drain(
        self, state: S.OrchestratorState, event: CompleteDrain, now: datetime
    ) -> Transition:
        lease = state.coordinator.lease
        if lease is None or lease.owner != event.owner or not _lease_active(lease, now):
            return Transition(state, {"kind": "conflict", "code": "lease_lost"}, False)
        return Transition(
            replace(
                state,
                coordinator=replace(state.coordinator, idle_since=S.instant(now)),
            ),
            {"kind": "accepted", "status": "idle_grace"},
        )

    def _tick(
        self, state: S.OrchestratorState, event: Tick, now: datetime
    ) -> Transition:
        coordinator = state.coordinator
        if coordinator.status == S.CoordinatorStatus.RUNNING and not _lease_active(
            coordinator.lease, now
        ):
            next_state = replace(
                state,
                coordinator=S.CoordinatorState(
                    status=S.CoordinatorStatus.WAKING.value,
                    generation=coordinator.generation,
                ),
            )
            next_state, wake = _wake(
                next_state,
                generation=coordinator.generation,
                reason="recovery",
                command_ref="expired-lease",
                now=now,
            )
            return Transition(
                next_state,
                {"kind": "accepted", "recovery_required": True, "wake_id": wake.id},
            )
        if (
            coordinator.status != S.CoordinatorStatus.RUNNING
            or coordinator.idle_since is None
        ):
            return Transition(
                state, {"kind": "accepted", "status": coordinator.status}, False
            )
        late = [
            command
            for command in state.commands.values()
            if command.status == S.CommandStatus.QUEUED.value
            and command.generation > coordinator.generation
        ]
        if late:
            generation = min(command.generation for command in late)
            next_state = replace(
                state,
                coordinator=S.CoordinatorState(
                    status=S.CoordinatorStatus.WAKING.value, generation=generation
                ),
            )
            next_state, wake = _wake(
                next_state,
                generation=generation,
                reason="commands_available",
                command_ref=late[0].command_id,
                now=now,
            )
            return Transition(next_state, {"kind": "accepted", "wake_id": wake.id})
        idle_at = S.parse_instant(coordinator.idle_since)
        if now - idle_at < timedelta(milliseconds=event.idle_grace_ms):
            return Transition(
                state, {"kind": "accepted", "status": "idle_grace"}, False
            )
        return Transition(
            replace(
                state,
                coordinator=S.CoordinatorState(
                    status=S.CoordinatorStatus.SLEEPING.value,
                    generation=coordinator.generation,
                ),
            ),
            {"kind": "accepted", "status": "sleeping"},
        )

    def _mark_dispatched(
        self, state: S.OrchestratorState, event: MarkDispatched, now: datetime
    ) -> Transition:
        entry = state.outbox.get(event.outbox_id)
        if entry is None:
            return Transition(
                state, {"kind": "rejected", "code": "unknown_outbox"}, False
            )
        if entry.external_id is not None:
            return Transition(
                state,
                {
                    "kind": "accepted"
                    if entry.external_id == event.external_id
                    else "conflict",
                    "code": "already_dispatched",
                },
                False,
            )
        updated = replace(
            entry, external_id=event.external_id, dispatched_at=S.instant(now)
        )
        return Transition(
            replace(state, outbox={**state.outbox, entry.id: updated}),
            {"kind": "accepted", "outbox_id": entry.id},
        )
