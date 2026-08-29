import json
import shutil
from datetime import UTC, datetime
from pathlib import Path

from charm.nightmare import aggregate as A
from charm.nightmare import build_bundle as B
from charm.nightmare import contracts
from charm.nightmare import executor as E
from charm.paths import nightmare_dir

MANIFEST_FIXTURE = (
    nightmare_dir() / "fixtures" / "contracts" / "valid" / "runner_manifest.json"
)
RESULT_FIXTURE = (
    nightmare_dir() / "fixtures" / "contracts" / "valid" / "runner_result.json"
)


def seed_bundle_inputs(root: Path, build_dir: Path) -> None:
    (build_dir / "kernel").mkdir(parents=True)
    (build_dir / "kernel" / "kernel").write_bytes(b"kernel")
    (build_dir / "d.img").write_bytes(b"disk")
    (root / "kernel").mkdir(parents=True)
    (root / "kernel" / "limine.conf").write_text(
        "/charmOS\n path: boot():/boot/kernel\n"
    )
    (root / "limine").mkdir()
    for name in (
        "limine",
        "limine-bios.sys",
        "limine-bios-cd.bin",
        "limine-uefi-cd.bin",
        "BOOTX64.EFI",
        "BOOTIA32.EFI",
    ):
        (root / "limine" / name).write_bytes(name.encode())


def generation(tmp_path: Path, manifest_ids: tuple[str, ...] = ("manifest_one",)):
    manifest_template = json.loads(MANIFEST_FIXTURE.read_text())
    config = manifest_template["suite"]["resolved"]["build"]
    source = manifest_template["source"]
    image = manifest_template["build"]["runner_image"]
    request_sha = contracts.sha256_json(
        {"source": source, "runner_image": image, "configuration": config}
    )
    request = B.BuildRequest(
        "build_contract_smoke",
        request_sha,
        source["repository"],
        source["commit"],
        image,
        config,
    )
    repo = tmp_path / "repo"
    build_dir = tmp_path / "build-input"
    seed_bundle_inputs(repo, build_dir)
    bundle = B.create_bundle(
        request,
        build_dir=build_dir,
        out_dir=tmp_path / "builds" / "bundle",
        repo_root=repo,
        now=datetime(2026, 8, 29, tzinfo=UTC),
        compile_kernel=False,
    )
    manifests_dir = tmp_path / "materialized" / "manifests"
    manifests_dir.mkdir(parents=True)
    entries = []
    requests = []
    for manifest_id in manifest_ids:
        manifest = json.loads(MANIFEST_FIXTURE.read_text())
        manifest["manifest_id"] = manifest_id
        manifest["build"]["sha256"] = bundle.sha256
        path = manifests_dir / f"{manifest_id}.json"
        path.write_bytes(contracts.canonical_json(manifest) + b"\n")
        entries.append(
            {
                "manifestId": manifest_id,
                "path": f"manifests/{manifest_id}.json",
                "sha256": contracts.sha256_file(path),
            }
        )
        requests.append({"manifestId": manifest_id, "buildGroupId": bundle.bundle_id})
    index = {
        "schemaVersion": 1,
        "planId": "plan_contract_smoke",
        "manifests": entries,
        "matrix": [],
    }
    index_path = tmp_path / "materialized" / "plan_bundle.json"
    index_path.write_text(json.dumps(index))
    plan = {
        "kind": "accepted",
        "plan": {
            "id": "plan_contract_smoke",
            "batch": {"id": "batch_contract_smoke"},
            "buildGroups": [
                {
                    "id": bundle.bundle_id,
                    "requestSha256": request_sha,
                    "configuration": config,
                }
            ],
            "manifests": requests,
        },
    }
    return plan, index_path, bundle, entries


def write_result(
    tmp_path: Path,
    entry: dict,
    *,
    discovery: str = "none",
    health: str = "healthy",
    lifecycle: str = "completed",
) -> Path:
    result_dir = tmp_path / "results" / entry["manifestId"]
    result_dir.mkdir(parents=True)
    source_manifest = tmp_path / "materialized" / entry["path"]
    shutil.copy2(source_manifest, result_dir / "manifest.json")
    manifest = json.loads(source_manifest.read_text())
    identity = {
        "schema_version": 1,
        "manifest_id": entry["manifestId"],
        "manifest_sha256": entry["sha256"],
        "source": manifest["source"],
        "suite": {
            "id": manifest["suite"]["id"],
            "sha256": manifest["suite"]["sha256"],
        },
        "build": manifest["build"],
    }
    (result_dir / "identity.json").write_text(json.dumps(identity))
    result = json.loads(RESULT_FIXTURE.read_text())
    result["manifest_id"] = entry["manifestId"]
    result["manifest_sha256"] = entry["sha256"]
    result["result_id"] = f"result_{entry['sha256'][:20]}"
    result["lifecycle"] = lifecycle
    result["execution"]["health"] = health
    result["execution"]["code"] = "completed" if health == "healthy" else "host_timeout"
    result["discovery"] = {
        "kind": discovery,
        "finding_count": 1 if discovery == "finding" else 0,
    }
    result["campaign"] = {
        "campaign_id": "campaign_contract_smoke",
        "trace": [{"at_ms": 0, "cumulative_progress": 0}],
        "findings": (
            [
                {
                    "sig": "stable-finding",
                    "tier": "confident",
                    "kind": "invariant",
                    "site": "fixture:1",
                    "msg": "canary",
                    "occurrences": 1,
                }
            ]
            if discovery == "finding"
            else []
        ),
    }
    (result_dir / "runner_result.json").write_text(json.dumps(result))
    return result_dir


def aggregate(tmp_path: Path, plan: dict, index: Path) -> A.AggregateReport:
    return A.aggregate(
        plan,
        results_dir=tmp_path / "results",
        plan_bundle_path=index,
        builds_dir=tmp_path / "builds",
    )


def test_normal_and_canary_results_stay_green(tmp_path: Path) -> None:
    plan, index, _bundle, entries = generation(
        tmp_path, ("manifest_normal", "manifest_canary")
    )
    write_result(tmp_path, entries[0])
    write_result(tmp_path, entries[1], discovery="finding")

    report = aggregate(tmp_path, plan, index)
    assert report.ok
    assert report.document["discovery"] == {
        "finding_count": 1,
        "unique_findings": 1,
    }
    assert "stable-finding" in A.render_markdown(report)
    assert "Replay" in A.render_markdown(report)


def test_interpreted_timeout_is_discovery_but_preboot_timeout_is_red(
    tmp_path: Path,
) -> None:
    plan, index, _bundle, entries = generation(tmp_path, ("manifest_timeout",))
    write_result(tmp_path, entries[0], discovery="stall")
    assert aggregate(tmp_path, plan, index).ok

    result_path = tmp_path / "results" / "manifest_timeout" / "runner_result.json"
    result = json.loads(result_path.read_text())
    result["lifecycle"] = "failed"
    result["execution"] = {
        "health": "infrastructure",
        "code": "host_timeout",
        "message": "no boot record",
    }
    result_path.write_text(json.dumps(result))
    report = aggregate(tmp_path, plan, index)
    assert not report.ok
    assert any(
        issue["code"] == "runner_infrastructure"
        for issue in report.document["infrastructure"]["issues"]
    )


def test_missing_duplicate_and_mismatched_results_are_partial(tmp_path: Path) -> None:
    plan, index, _bundle, entries = generation(tmp_path, ("manifest_a", "manifest_b"))
    first = write_result(tmp_path, entries[0])
    duplicate = tmp_path / "results" / "duplicate"
    shutil.copytree(first, duplicate)
    shutil.copytree(tmp_path / "builds" / "bundle", tmp_path / "builds" / "duplicate")
    result_path = first / "runner_result.json"
    result = json.loads(result_path.read_text())
    result["manifest_sha256"] = "f" * 64
    result_path.write_text(json.dumps(result))
    (duplicate / "runner_result.json").write_text(json.dumps(result))

    report = aggregate(tmp_path, plan, index)
    codes = {item["code"] for item in report.document["infrastructure"]["issues"]}
    assert not report.ok and report.partial
    assert {
        "missing_runner_result",
        "duplicate_runner_result",
        "result_identity_mismatch",
        "duplicate_build_bundle",
    } <= codes


def test_replay_resolves_the_retained_manifest(tmp_path: Path) -> None:
    plan, index, _bundle, entries = generation(tmp_path)
    result_dir = write_result(tmp_path, entries[0], discovery="finding")
    assert aggregate(tmp_path, plan, index).ok
    assert E.resolve_replay_manifest(result_dir) == result_dir / "manifest.json"
