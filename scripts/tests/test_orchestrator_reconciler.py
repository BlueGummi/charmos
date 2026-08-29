import json
from datetime import UTC, datetime, timedelta

from charm.nightmare import contracts
from charm.orchestrator import reconciler as R
from charm.orchestrator import state as S
from charm.orchestrator.coordinator import Coordinator
from charm.orchestrator.store import MemoryStore, PostgresStore
from charm.orchestrator.transport import GitHubActionsTransport, MemoryTransport


class Clock:
    def __init__(self) -> None:
        self.now = datetime(2026, 8, 29, tzinfo=UTC)

    def __call__(self) -> datetime:
        return self.now


def command(command_id: str = "command_one", key: str = "idempotency-one") -> dict:
    return {
        "schema_version": 1,
        "command_id": command_id,
        "operation": "validate_batch",
        "idempotency_key": key,
        "actor": {"id": "1", "login": "operator"},
        "repository": {"id": "axvonx/charmos", "ref": "main"},
        "payload": {},
    }


def setup() -> tuple[Clock, MemoryStore, MemoryTransport, Coordinator]:
    clock = Clock()
    store = MemoryStore(clock=clock)
    transport = MemoryTransport()
    return clock, store, transport, Coordinator(store, transport)


def test_duplicate_command_and_dispatch_are_idempotent() -> None:
    _clock, store, transport, coordinator = setup()
    first = coordinator.apply(R.SubmitCommand(command()))
    second = coordinator.apply(R.SubmitCommand(command()))

    state = store.read().state
    assert first.result["kind"] == "accepted"
    assert second.result["command_id"] == "command_one"
    assert len(state.commands) == 1
    assert len(state.outbox) == 1
    assert len(transport.dispatched) == 1

    conflicting = command("command_other", "idempotency-one")
    result = coordinator.apply(R.SubmitCommand(conflicting))
    assert result.result["code"] == "idempotency_conflict"
    assert len(store.read().state.commands) == 1


def test_coordinator_lease_expires_and_recovery_wakes_once() -> None:
    clock, store, transport, coordinator = setup()
    submitted = coordinator.apply(R.SubmitCommand(command()))
    wake_id = str(submitted.result["wake_id"])
    acquired = coordinator.apply(R.AcquireCoordinator(wake_id, "worker-a", 1_000))
    assert acquired.result["lease"] == "acquired"
    conflict = coordinator.apply(R.AcquireCoordinator(wake_id, "worker-b", 1_000))
    assert conflict.result["code"] == "coordinator_leased"

    clock.now += timedelta(seconds=2)
    recovered = coordinator.apply(R.Tick(idle_grace_ms=5_000))
    assert recovered.result["recovery_required"] is True
    assert store.read().state.coordinator.status == "waking"
    assert len(transport.dispatched) == 2


def test_consumed_wake_cannot_create_duplicate_execution() -> None:
    clock, store, _transport, coordinator = setup()
    submitted = coordinator.apply(R.SubmitCommand(command()))
    wake_id = str(submitted.result["wake_id"])
    coordinator.apply(R.AcquireCoordinator(wake_id, "worker", 60_000))
    coordinator.apply(R.CompleteDrain("worker"))
    clock.now += timedelta(seconds=31)
    coordinator.apply(R.Tick(idle_grace_ms=30_000))

    duplicate = coordinator.apply(R.AcquireCoordinator(wake_id, "worker-2", 60_000))
    assert duplicate.result == {"kind": "rejected", "code": "wake_consumed"}
    assert store.read().state.coordinator.status == "sleeping"


def test_late_command_enters_next_generation_after_idle_grace() -> None:
    _clock, store, transport, coordinator = setup()
    first = coordinator.apply(R.SubmitCommand(command()))
    coordinator.apply(
        R.AcquireCoordinator(str(first.result["wake_id"]), "worker", 60_000)
    )
    late = coordinator.apply(
        R.SubmitCommand(command("command_late", "idempotency-late"))
    )
    assert late.result["generation"] == 2
    assert len(transport.dispatched) == 1

    coordinator.apply(R.CompleteDrain("worker"))
    next_wake = coordinator.apply(R.Tick(idle_grace_ms=30_000))
    assert next_wake.result["wake_id"]
    assert store.read().state.coordinator.generation == 2
    assert len(transport.dispatched) == 2


def test_partial_result_and_artifact_loss_schedule_exact_recovery() -> None:
    clock, store, transport, coordinator = setup()
    submitted = coordinator.apply(R.SubmitCommand(command()))
    coordinator.apply(
        R.AcquireCoordinator(str(submitted.result["wake_id"]), "worker", 60_000)
    )
    plan = {"kind": "accepted", "plan": {"id": "plan_one"}}
    coordinator.apply(R.RecordPlan("command_one", plan, {"version": "fleet_one"}))
    coordinator.apply(
        R.RegisterAttempt("attempt_one", "plan_one", ("manifest_a", "manifest_b"))
    )
    first = coordinator.apply(R.RecordResult("attempt_one", "manifest_a", "a" * 64))
    assert first.result["complete"] is False
    duplicate = coordinator.apply(R.RecordResult("attempt_one", "manifest_a", "a" * 64))
    assert duplicate.result["code"] == "duplicate_result"
    mismatch = coordinator.apply(R.RecordResult("attempt_one", "manifest_a", "b" * 64))
    assert mismatch.result["code"] == "result_mismatch"

    lost = coordinator.apply(R.ArtifactLost("attempt_one", "manifest_a"))
    attempt = store.read().state.attempts["attempt_one"]
    assert lost.result["recovery_required"] is True
    assert attempt.status == "recovery_required"
    assert attempt.missing_artifacts == ("manifest_a",)
    assert len(transport.dispatched) == 2

    clock.now += timedelta(seconds=1)
    replayed = coordinator.apply(R.RecordResult("attempt_one", "manifest_b", "c" * 64))
    assert replayed.result["complete"] is False
    recovered = coordinator.apply(R.RecordResult("attempt_one", "manifest_a", "a" * 64))
    assert recovered.result == {"kind": "accepted", "complete": True, "recovered": True}


def test_state_round_trips_without_losing_typed_records() -> None:
    _clock, store, _transport, coordinator = setup()
    coordinator.apply(R.SubmitCommand(command()))
    state = store.read().state
    assert S.OrchestratorState.from_dict(state.to_dict()) == state


def test_github_adapter_sends_only_the_doorbell_contract() -> None:
    captured = {}

    class Response:
        status = 204

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

    def opener(request, timeout):
        captured["url"] = request.full_url
        captured["body"] = json.loads(request.data)
        captured["timeout"] = timeout
        return Response()

    wake = S.OutboxEntry(
        "wake_1_fixture",
        1,
        "commands_available",
        "command_one",
        S.instant(datetime(2026, 8, 29, tzinfo=UTC)),
    )
    adapter = GitHubActionsTransport(
        repository="axvonx/charmos",
        workflow="nightmare-orchestrator.yml",
        ref="main",
        token="short-lived",
        opener=opener,
    )
    receipt = adapter.dispatch(wake)

    assert captured["body"] == {
        "ref": "main",
        "inputs": {
            "protocol_version": "1",
            "wake_id": "wake_1_fixture",
            "reason": "commands_available",
            "command_ref": "command_one",
        },
    }
    assert receipt.external_id == "github:axvonx/charmos:wake_1_fixture"
    assert contracts.sha256_json(captured["body"])


def test_postgres_adapter_uses_serializable_compare_and_swap() -> None:
    now = datetime(2026, 8, 29, tzinfo=UTC)
    executions: list[tuple[str, object]] = []
    rows = [
        (0, S.OrchestratorState().to_dict(), now),
        (1, now),
    ]

    class Cursor:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def execute(self, sql, parameters=None):
            executions.append((sql, parameters))

        def fetchone(self):
            return rows.pop(0)

    class Connection:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def cursor(self):
            return Cursor()

        def rollback(self):
            raise AssertionError("CAS unexpectedly lost")

    store = PostgresStore(Connection)
    envelope = store.read()
    committed = store.compare_and_swap(envelope.version, envelope.state)

    assert committed is not None and committed.version == 1
    assert any("CURRENT_TIMESTAMP" in sql for sql, _params in executions)
    assert any("SERIALIZABLE" in sql for sql, _params in executions)
    update = next(item for item in executions if "UPDATE nightmare" in item[0])
    assert update[1][2] == 0
