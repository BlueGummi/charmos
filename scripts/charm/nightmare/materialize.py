"""Accepted plan plus verified build receipts to byte-stable manifests."""

import json
from pathlib import Path
from typing import Any

from . import contracts, domain


class MaterializationError(ValueError):
    pass


def _accepted_plan(document: dict[str, Any]) -> dict[str, Any]:
    if document.get("kind") == "accepted" and isinstance(document.get("plan"), dict):
        return document["plan"]
    raise MaterializationError("input is not an accepted plan v1")


def load_plan(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MaterializationError(f"cannot read accepted plan: {error}") from None
    if not isinstance(document, dict):
        raise MaterializationError("accepted plan must be an object")
    _accepted_plan(document)
    return document


def load_receipt(path: Path) -> domain.BundleReceipt:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        return domain.BundleReceipt(
            bundle_id=document["bundle_id"],
            sha256=document["sha256"],
            request_sha256=document["request_sha256"],
        )
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise MaterializationError(
            f"cannot read build receipt {path}: {error}"
        ) from None


def materialize(
    plan_document: dict[str, Any],
    receipts: tuple[domain.BundleReceipt, ...],
    *,
    attempt: int = 1,
    dry_run: bool = False,
) -> domain.PlanBundle:
    """Materialize all runner manifests; no caller constructs one piecemeal."""
    plan = _accepted_plan(plan_document)
    if attempt < 1:
        raise MaterializationError("attempt must be at least 1")

    groups = {group["id"]: group for group in plan["buildGroups"]}
    receipt_by_id = {receipt.bundle_id: receipt for receipt in receipts}
    if set(receipt_by_id) != set(groups):
        missing = sorted(set(groups) - set(receipt_by_id))
        unexpected = sorted(set(receipt_by_id) - set(groups))
        raise MaterializationError(
            f"build receipt set mismatch (missing={missing}, unexpected={unexpected})"
        )
    for group_id, group in groups.items():
        receipt = receipt_by_id[group_id]
        if receipt.request_sha256 != group["requestSha256"]:
            raise MaterializationError(
                f"{group_id}: build request digest does not match accepted plan"
            )

    suite_by_id = {suite["id"]: suite for suite in plan["suites"]}
    manifests: list[dict[str, Any]] = []
    for request in sorted(plan["manifests"], key=lambda item: item["manifestId"]):
        suite = suite_by_id[request["suiteId"]]
        receipt = receipt_by_id[request["buildGroupId"]]
        resolved_tasks = suite["resolved"]["tasks"]
        gate_first = any(task["boot"]["gate_first"] for task in resolved_tasks)
        manifest = {
            "schema_version": 1,
            "manifest_id": request["manifestId"],
            "plan_id": plan["id"],
            "batch_id": plan["batch"]["id"],
            "task_id": request["taskId"],
            "attempt": attempt,
            "source": plan["source"],
            "suite": suite,
            "build": {
                "bundle_id": receipt.bundle_id,
                "sha256": receipt.sha256,
                "runner_image": plan["runnerImage"],
            },
            "campaign": {
                "campaign_id": request["campaignId"],
                "runner_index": request["runnerIndex"],
                "total_runners": request["totalRunners"],
                "base_seed": request["baseSeed"],
                "soft_budget_ms": request["softBudgetMs"],
                "hard_budget_ms": request["hardBudgetMs"],
                "actions_job_budget_ms": request["actionsJobBudgetMs"],
                "gate_first": gate_first,
                "dry_run": dry_run,
            },
            "result": {
                "schema_version": 1,
                "artifact_name": f"nightmare-result-{request['manifestId']}",
            },
        }
        diagnostics = contracts.validate_manifest(manifest)
        if diagnostics:
            detail = "; ".join(str(item) for item in diagnostics)
            raise MaterializationError(
                f"{request['manifestId']}: generated invalid manifest: {detail}"
            )
        manifests.append(manifest)

    build_groups = tuple(
        domain.BuildGroup(
            id=group["id"],
            request_sha256=group["requestSha256"],
            configuration=group["configuration"],
            task_ids=tuple(group["taskIds"]),
        )
        for group in sorted(groups.values(), key=lambda item: item["id"])
    )
    return domain.PlanBundle(plan["id"], build_groups, tuple(manifests))


def write_bundle(bundle: domain.PlanBundle, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_dir = out_dir / "manifests"
    manifest_dir.mkdir(parents=True, exist_ok=True)
    manifest_entries = []
    for manifest in bundle.manifests:
        path = manifest_dir / f"{manifest['manifest_id']}.json"
        path.write_bytes(contracts.canonical_json(manifest) + b"\n")
        manifest_entries.append(
            {
                "manifestId": manifest["manifest_id"],
                "path": str(path.relative_to(out_dir)),
                "sha256": contracts.sha256_file(path),
            }
        )
    index = {
        "schemaVersion": 1,
        "planId": bundle.plan_id,
        "manifests": manifest_entries,
        "matrix": list(bundle.matrix),
    }
    index_path = out_dir / "plan_bundle.json"
    index_path.write_bytes(contracts.canonical_json(index) + b"\n")
    return index_path
