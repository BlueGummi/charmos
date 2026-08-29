from datetime import UTC, datetime

from charm.orchestrator.coordinator import Coordinator
from charm.orchestrator.operator import Audit, FleetQuery, Operator, OperatorConfig
from charm.orchestrator.planner import Planner
from charm.orchestrator.runtime import ExecutionRuntime
from charm.orchestrator.store import MemoryStore
from charm.orchestrator.transport import MemoryTransport
from charm.paths import nightmare_dir

NOW = datetime(2026, 8, 29, tzinfo=UTC)


def setup() -> tuple[Operator, MemoryStore, MemoryTransport]:
    store = MemoryStore(clock=lambda: NOW)
    transport = MemoryTransport()
    operator = Operator(
        Coordinator(store, transport),
        Planner(nightmare_dir() / "suites"),
        OperatorConfig(
            repository_id="charmos",
            repository="axvonx/charmos",
            owner="axvonx",
            name="charmos",
            default_ref="main",
            refs=("main",),
            source_commit="56bc1dfdd14a0b3e03ff1d166e7af98bec8cbb6f",
            runner_image="ghcr.io/axvonx/charmos-x86-env@sha256:" + "2" * 64,
            runner_capacity=4,
        ),
    )
    return operator, store, transport


def audit(request_id: str = "request-one") -> Audit:
    return Audit("42", "operator", request_id)


def definition(version: str) -> dict:
    return {
        "definition": {
            "name": "Operator smoke",
            "startUtc": "2026-08-29T01:00:00Z",
            "windowHours": 1,
            "runners": 2,
            "color": "#a7c080",
            "tests": ["harness_smoke", "p7_m3_short"],
        },
        "definitionToml": (
            "[batch]\n"
            'name = "Operator smoke"\n'
            'start_utc = "2026-08-29T01:00:00Z"\n'
            "window_hours = 1\n"
            "runners = 2\n"
            'color = "#a7c080"\n'
            'tests = ["harness_smoke", "p7_m3_short"]\n'
        ),
        "baseSnapshotVersion": version,
        "idempotencyKey": "validate-operator-one",
    }


def test_six_operation_interface_is_durable_idempotent_and_audited() -> None:
    operator, store, transport = setup()
    snapshot = operator.get_fleet_window(
        FleetQuery(
            "charmos",
            "main",
            NOW,
            datetime(2026, 8, 30, tzinfo=UTC),
        )
    )
    validated = operator.validate_batch(definition(snapshot["version"]), audit())
    assert validated["kind"] == "accepted"
    assert transport.dispatched == {}

    replayed = operator.validate_batch(definition(snapshot["version"]), audit("retry"))
    assert replayed == validated
    validate_command = next(iter(store.read().state.commands.values()))
    assert validate_command.document["actor"] == {"id": "42", "login": "operator"}
    assert validate_command.request_id == "request-one"

    plan = validated["plan"]
    submitted = operator.submit_batch(
        {
            "planId": plan["id"],
            "planVersion": plan["version"],
            "baseSnapshotVersion": plan["baseSnapshotVersion"],
            "idempotencyKey": "submit-operator-one",
        },
        audit("request-submit"),
    )
    assert submitted["kind"] == "accepted"
    assert submitted["batch"]["lifecycle"] == "queued"
    assert operator.get_batch(submitted["batch"]["id"]) == submitted["batch"]
    assert len(transport.dispatched) == 1

    mutable = submitted["batch"]["residualTail"]["mutableTaskIds"]
    drafted = operator.draft_tail_update(
        {
            "batchId": submitted["batch"]["id"],
            "baseBatchVersion": submitted["batch"]["version"],
            "cancelledTaskIds": [mutable[0]],
            "idempotencyKey": "tail-draft-operator-one",
        },
        audit("request-tail-draft"),
    )
    assert drafted["kind"] == "accepted"
    assert next(
        task for task in drafted["plan"]["batch"]["tasks"] if task["id"] == mutable[0]
    )["cancelled"]

    tail_plan = drafted["plan"]
    committed = operator.commit_tail_update(
        {
            "planId": tail_plan["id"],
            "planVersion": tail_plan["version"],
            "baseBatchVersion": tail_plan["baseBatchVersion"],
            "idempotencyKey": "tail-commit-operator-one",
        },
        audit("request-tail-commit"),
    )
    assert committed["kind"] == "accepted"
    assert committed["batch"]["version"] == tail_plan["batch"]["version"]
    assert committed["snapshot"]["version"] != submitted["snapshot"]["version"]
    assert len(transport.dispatched) == 1


def test_stale_snapshot_and_tail_conflicts_are_typed_results() -> None:
    operator, _store, _transport = setup()
    first = operator.get_fleet_window(
        FleetQuery("charmos", "main", NOW, datetime(2026, 8, 30, tzinfo=UTC))
    )
    validated = operator.validate_batch(definition(first["version"]), audit())
    plan = validated["plan"]
    submitted = operator.submit_batch(
        {
            "planId": plan["id"],
            "planVersion": plan["version"],
            "baseSnapshotVersion": plan["baseSnapshotVersion"],
            "idempotencyKey": "submit-stale-one",
        },
        audit("submit"),
    )

    stale_body = definition(first["version"])
    stale_body["idempotencyKey"] = "validate-stale-one"
    stale = operator.validate_batch(stale_body, audit("stale"))
    assert stale["kind"] == "stale"
    assert stale["currentVersion"] == submitted["snapshot"]["version"]

    conflict = operator.draft_tail_update(
        {
            "batchId": submitted["batch"]["id"],
            "baseBatchVersion": "batchv_stale",
            "cancelledTaskIds": [
                submitted["batch"]["residualTail"]["mutableTaskIds"][0]
            ],
            "idempotencyKey": "tail-conflict-one",
        },
        audit("conflict"),
    )
    assert conflict["kind"] == "conflict"
    assert conflict["currentVersion"] == submitted["batch"]["version"]


def test_submitted_plan_runs_through_durable_prepare_and_finish() -> None:
    operator, store, _transport = setup()
    snapshot = operator.get_fleet_window(
        FleetQuery("charmos", "main", NOW, datetime(2026, 8, 30, tzinfo=UTC))
    )
    accepted = operator.validate_batch(definition(snapshot["version"]), audit())["plan"]
    operator.submit_batch(
        {
            "planId": accepted["id"],
            "planVersion": accepted["version"],
            "baseSnapshotVersion": accepted["baseSnapshotVersion"],
            "idempotencyKey": "submit-runtime-one",
        },
        audit("runtime-submit"),
    )
    state = store.read().state
    wake = next(iter(state.outbox.values()))
    runtime = ExecutionRuntime(operator)
    prepared = runtime.prepare(
        wake_id=wake.id,
        command_ref=wake.command_ref,
        owner="github:run-one",
    )
    plan = prepared.plan["plan"]
    rows = [
        {
            "manifest_id": manifest["manifestId"],
            "state": "completed",
            "health": "healthy",
            "discovery": "none",
            "trace": [
                {"at_ms": 0, "cumulative_progress": 0},
                {"at_ms": 1000, "cumulative_progress": 12},
            ],
        }
        for manifest in plan["manifests"]
    ]
    report = {
        "plan_id": plan["id"],
        "batch_id": plan["batch"]["id"],
        "partial": False,
        "results": rows,
    }
    finished = runtime.finish(
        attempt_id=prepared.attempt_id,
        report=report,
        result_digests={row["manifest_id"]: "a" * 64 for row in rows},
        owner="github:run-one",
    )

    assert finished["batch"]["lifecycle"] == "completed"
    assert all(task["state"] == "passed" for task in finished["batch"]["tasks"])
    assert store.read().state.coordinator.status == "sleeping"
