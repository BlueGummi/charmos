import json
from datetime import UTC, datetime
from pathlib import Path

import pytest

from charm.cli import build_parser
from charm.nightmare import contracts
from charm.nightmare import domain as D
from charm.nightmare import materialize as M
from charm.nightmare import planner as P
from charm.nightmare import workflow as W
from charm.paths import nightmare_dir

NOW = datetime(2026, 8, 29, 0, 0, tzinfo=UTC)
COMMIT = "56bc1dfdd14a0b3e03ff1d166e7af98bec8cbb6f"
IMAGE = "ghcr.io/axvonx/charmos-x86-env@sha256:" + "2" * 64
ORCHESTRATOR_FIXTURES = nightmare_dir() / "fixtures" / "orchestrator"


def fleet() -> dict:
    return {
        "schemaVersion": 1,
        "version": "fleet_test_v1",
        "evaluatedAt": "2026-08-29T00:00:00Z",
        "window": {
            "startsAt": "2026-08-29T00:00:00Z",
            "endsAt": "2026-08-30T00:00:00Z",
        },
        "repository": {
            "id": "charmos",
            "owner": "axvonx",
            "name": "charmos",
            "ref": "main",
            "refs": ["main"],
        },
        "runners": [
            {
                "id": f"runner_{index}",
                "number": index,
                "state": "idle",
                "label": f"Runner {index}",
            }
            for index in range(1, 5)
        ],
        "batches": [],
        "partial": False,
    }


def command(
    tests: list[str] | None = None,
    *,
    runners: int = 2,
    base_version: str = "fleet_test_v1",
) -> dict:
    tests = tests or ["harness_smoke"]
    definition = {
        "name": "Planner Test",
        "start_utc": "2026-08-29T01:00:00Z",
        "window_hours": 1,
        "runners": runners,
        "color": "#a7c080",
        "tests": tests,
    }
    toml_tests = ", ".join(f'"{item}"' for item in tests)
    source = (
        "[batch]\n"
        'name = "Planner Test"\n'
        'start_utc = "2026-08-29T01:00:00Z"\n'
        "window_hours = 1\n"
        f"runners = {runners}\n"
        'color = "#a7c080"\n'
        f"tests = [{toml_tests}]\n"
    )
    return {
        "schema_version": 1,
        "command_id": "command_planner_test",
        "operation": "validate_batch",
        "idempotency_key": "planner-test-key",
        "actor": {"id": "1", "login": "tester"},
        "repository": {"id": "axvonx/charmos", "ref": "main"},
        "payload": {
            "definition": definition,
            "definition_toml": source,
            "base_snapshot_version": base_version,
        },
    }


def planner(max_manifests: int = 64) -> P.Planner:
    return P.Planner(nightmare_dir() / "suites", max_manifests=max_manifests)


def plan_result(command_doc: dict | None = None, snapshot: dict | None = None):
    return planner().plan(
        command_doc or command(),
        snapshot or fleet(),
        source=D.Source("axvonx/charmos", COMMIT),
        runner_image=IMAGE,
        now=NOW,
    )


def test_planner_is_deterministic_and_resolves_real_suites() -> None:
    first = plan_result()
    second = plan_result()

    assert isinstance(first, D.Accepted)
    assert isinstance(second, D.Accepted)
    assert contracts.canonical_json(first.plan.document) == contracts.canonical_json(
        second.plan.document
    )
    assert first.plan.suites[0].id == "harness_smoke"
    assert len(first.plan.tasks) == 1
    assert first.plan.tasks[0].runner_id == "runner_1"


def test_generated_plan_satisfies_the_published_contract() -> None:
    jsonschema = pytest.importorskip("jsonschema")
    referencing = pytest.importorskip("referencing")
    result = plan_result()
    assert isinstance(result, D.Accepted)
    schema_dir = nightmare_dir() / "schemas"
    schemas = [
        json.loads((schema_dir / name).read_text())
        for name in (
            "accepted-plan-v1.schema.json",
            "fleet-snapshot-v1.schema.json",
        )
    ]
    registry = referencing.Registry().with_resources(
        (schema["$id"], referencing.Resource.from_contents(schema))
        for schema in schemas
    )
    validator = jsonschema.Draft202012Validator(
        schemas[0],
        registry=registry,
        format_checker=jsonschema.FormatChecker(),
    )

    assert list(validator.iter_errors(result.plan.document)) == []


def test_duplicate_display_names_keep_distinct_durable_ids() -> None:
    result = plan_result(command(["harness_smoke", "harness_smoke"]))
    assert isinstance(result, D.Accepted)
    tasks = result.plan.document["batch"]["tasks"]

    assert [task["name"] for task in tasks] == ["harness_smoke", "harness_smoke"]
    assert [task["instance"] for task in tasks] == [1, 2]
    assert len({task["id"] for task in tasks}) == 2
    assert len({item["manifestId"] for item in result.plan.document["manifests"]}) == 2


def test_stale_unknown_suite_and_bounded_matrix_are_typed_results() -> None:
    stale = plan_result(command(base_version="fleet_old"))
    unknown = plan_result(command(["not_registered"]))
    bounded = planner(max_manifests=1).plan(
        command(["harness_smoke", "harness_smoke"]),
        fleet(),
        source=D.Source("axvonx/charmos", COMMIT),
        runner_image=IMAGE,
        now=NOW,
    )

    assert isinstance(stale, D.Stale)
    assert stale.current_version == "fleet_test_v1"
    assert isinstance(unknown, D.Rejected)
    assert unknown.code == "invalid_definition"
    assert unknown.diagnostics[0]["field"] == "tests[0]"
    assert isinstance(bounded, D.Rejected)
    assert "limit is 1" in bounded.diagnostics[0]["message"]


def test_capacity_requires_one_contiguous_runner_lease() -> None:
    snapshot = fleet()
    snapshot["batches"] = [
        {
            "lease": {
                "runnerIds": ["runner_2"],
                "startsAt": "2026-08-29T00:30:00Z",
                "endsAt": "2026-08-29T03:00:00Z",
            }
        }
    ]
    result = plan_result(command(runners=3), snapshot)

    assert isinstance(result, D.Rejected)
    assert result.code == "no_capacity"
    assert result.alternatives[0]["startsAt"] == "2026-08-29T03:00:00Z"


def test_repository_plan_has_no_mutable_residual_tail() -> None:
    result = planner().plan(
        command(),
        fleet(),
        source=D.Source("axvonx/charmos", COMMIT),
        runner_image=IMAGE,
        now=NOW,
        ownership="repository",
    )
    assert isinstance(result, D.Accepted)
    assert result.plan.document["batch"]["residualTail"] is None


def test_materializer_requires_exact_build_receipts_and_is_byte_stable(
    tmp_path: Path,
) -> None:
    result = plan_result(command(["harness_smoke", "p7_m3_short"]))
    assert isinstance(result, D.Accepted)
    receipts = tuple(
        D.BundleReceipt(
            group.id, contracts.sha256_json(group.configuration), group.request_sha256
        )
        for group in result.plan.build_groups
    )

    accepted_document = P.render_result(result)
    bundle = M.materialize(accepted_document, receipts, dry_run=True)
    first = M.write_bundle(bundle, tmp_path / "first")
    second = M.write_bundle(bundle, tmp_path / "second")

    assert first.read_bytes() == second.read_bytes()
    assert len(bundle.manifests) == 2
    assert all(contracts.validate_manifest(item) == [] for item in bundle.manifests)
    assert bundle.matrix == tuple(
        sorted(bundle.matrix, key=lambda item: item["manifest_id"])
    )

    bad = list(receipts)
    bad[0] = D.BundleReceipt(bad[0].bundle_id, bad[0].sha256, "0" * 64)
    try:
        M.materialize(accepted_document, tuple(bad))
    except M.MaterializationError as error:
        assert "does not match accepted plan" in str(error)
    else:
        raise AssertionError("mismatched build receipt was accepted")

    with pytest.raises(M.MaterializationError, match="not an accepted plan"):
        M.materialize(result.plan.document, receipts)


def test_definition_toml_cannot_disagree_with_normalized_command() -> None:
    command_doc = command()
    command_doc["payload"]["definition_toml"] = command_doc["payload"][
        "definition_toml"
    ].replace("runners = 2", "runners = 1")
    result = plan_result(command_doc)

    assert isinstance(result, D.Rejected)
    assert result.diagnostics[0]["field"] == "definition_toml"


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("window_hours", float("inf")),
        ("runners", "two"),
        ("tests", []),
    ],
)
def test_malformed_definition_is_a_structured_rejection(
    field: str, value: object
) -> None:
    command_doc = command()
    command_doc["payload"]["definition"][field] = value
    result = plan_result(command_doc)

    assert isinstance(result, D.Rejected)
    assert result.code == "invalid_definition"
    assert result.diagnostics[0]["field"] == "command"


@pytest.mark.parametrize(
    ("command_name", "snapshot_name", "result_type", "code"),
    [
        ("validate_harness.json", "fleet_open.json", D.Accepted, None),
        ("validate_duplicate.json", "fleet_open.json", D.Accepted, None),
        ("validate_p7_duplicate.json", "fleet_open.json", D.Accepted, None),
        ("validate_stale.json", "fleet_open.json", D.Stale, "snapshot_changed"),
        ("validate_unknown.json", "fleet_open.json", D.Rejected, "invalid_definition"),
        ("validate_capacity.json", "fleet_blocked.json", D.Rejected, "no_capacity"),
        ("validate_harness.json", "fleet_blocked.json", D.Accepted, None),
    ],
)
def test_checked_in_planner_fixtures(
    command_name: str,
    snapshot_name: str,
    result_type: type,
    code: str | None,
) -> None:
    command_doc = json.loads((ORCHESTRATOR_FIXTURES / command_name).read_text())
    snapshot_doc = json.loads((ORCHESTRATOR_FIXTURES / snapshot_name).read_text())
    result = planner().plan(
        command_doc,
        snapshot_doc,
        source=D.Source("axvonx/charmos", COMMIT),
        runner_image=IMAGE,
        now=NOW,
    )
    assert isinstance(result, result_type)
    if code is not None:
        assert result.code == code


def test_repository_workflow_adapter_is_deterministic_and_bounded() -> None:
    first_command, first_snapshot = W.repository_command(
        suite_id="harness_smoke",
        suite_dir=nightmare_dir() / "suites",
        repository="axvonx/charmos",
        ref="main",
        now=NOW,
    )
    second_command, second_snapshot = W.repository_command(
        suite_id="harness_smoke",
        suite_dir=nightmare_dir() / "suites",
        repository="axvonx/charmos",
        ref="main",
        now=NOW,
    )
    assert first_command == second_command
    assert first_snapshot == second_snapshot
    result = planner().plan(
        first_command,
        first_snapshot,
        source=D.Source("axvonx/charmos", COMMIT),
        runner_image=IMAGE,
        now=NOW,
        ownership="repository",
    )
    assert isinstance(result, D.Accepted)
    rows = W.build_matrix(P.render_result(result))
    assert rows == [
        {
            "bundle_id": result.plan.build_groups[0].id,
            "artifact": f"nightmare-build-{result.plan.build_groups[0].id}",
        }
    ]


def test_phase_five_commands_are_exposed_by_the_cli() -> None:
    parser = build_parser()
    assert (
        parser.parse_args(
            [
                "nightmare",
                "repository-command",
                "--suite",
                "harness_smoke",
                "--repository",
                "axvonx/charmos",
                "--command-out",
                "command.json",
                "--snapshot-out",
                "snapshot.json",
            ]
        ).fn.__name__
        == "cmd_nm_repository_command"
    )
    assert (
        parser.parse_args(["nightmare", "matrix", "build", "plan.json"]).fn.__name__
        == "cmd_nm_matrix"
    )
    assert (
        parser.parse_args(
            [
                "nightmare",
                "aggregate",
                "plan.json",
                "results",
                "--json",
                "report.json",
                "--markdown",
                "report.md",
            ]
        ).fn.__name__
        == "cmd_nm_aggregate"
    )
