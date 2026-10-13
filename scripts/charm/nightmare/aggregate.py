"""Completeness-first aggregation for manifest-driven Nightmare executions."""

import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from .. import report as RP
from . import build_bundle, contracts, executor


class AggregateError(ValueError):
    pass


@dataclass(frozen=True)
class AggregateReport:
    document: dict[str, Any]

    @property
    def ok(self) -> bool:
        return bool(self.document["ok"])

    @property
    def partial(self) -> bool:
        return bool(self.document["partial"])


def _issue(code: str, subject: object, message: str) -> dict[str, str]:
    return {"code": code, "subject": str(subject), "message": message}


def _read_object(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AggregateError(f"cannot read {path}: {error}") from None
    if not isinstance(document, dict):
        raise AggregateError(f"{path}: expected a JSON object")
    return document


def _plan(document: dict[str, Any]) -> dict[str, Any]:
    if document.get("kind") != "accepted" or not isinstance(document.get("plan"), dict):
        raise AggregateError("aggregate requires an accepted plan result")
    return document["plan"]


def _safe_relative(root: Path, value: str) -> Path:
    pure = PurePosixPath(value)
    if pure.is_absolute() or ".." in pure.parts:
        raise AggregateError(f"unsafe artifact path: {value!r}")
    path = root.joinpath(*pure.parts).resolve()
    if not path.is_relative_to(root.resolve()):
        raise AggregateError(f"artifact path escapes its root: {value!r}")
    return path


def _expected_manifests(
    plan: dict[str, Any], plan_bundle_path: Path | None
) -> tuple[dict[str, dict[str, Any]], list[dict[str, str]]]:
    expected = {
        item["manifestId"]: {
            "manifest_id": item["manifestId"],
            "bundle_id": item["buildGroupId"],
            "bundle_sha256": None,
            "sha256": None,
            "manifest": None,
        }
        for item in plan["manifests"]
    }
    issues: list[dict[str, str]] = []
    if plan_bundle_path is None or not plan_bundle_path.is_file():
        issues.append(
            _issue(
                "missing_plan_bundle", plan["id"], "accepted plan was not materialized"
            )
        )
        return expected, issues

    index = _read_object(plan_bundle_path)
    if index.get("planId") != plan["id"]:
        issues.append(
            _issue(
                "plan_identity_mismatch",
                str(index.get("planId")),
                "materialized plan ID does not match the accepted plan",
            )
        )
        return expected, issues
    seen: set[str] = set()
    for item in index.get("manifests", []):
        manifest_id = item.get("manifestId")
        if manifest_id in seen:
            issues.append(
                _issue(
                    "duplicate_manifest",
                    str(manifest_id),
                    "manifest appears more than once in the plan bundle",
                )
            )
            continue
        seen.add(manifest_id)
        if manifest_id not in expected:
            issues.append(
                _issue(
                    "unexpected_manifest",
                    str(manifest_id),
                    "plan bundle contains an unplanned manifest",
                )
            )
            continue
        try:
            path = _safe_relative(plan_bundle_path.parent, item["path"])
        except (KeyError, TypeError, AggregateError) as error:
            issues.append(_issue("invalid_manifest_path", str(manifest_id), str(error)))
            continue
        if not path.is_file() or contracts.sha256_file(path) != item.get("sha256"):
            issues.append(
                _issue(
                    "manifest_digest_mismatch",
                    str(manifest_id),
                    "materialized manifest is missing or has the wrong digest",
                )
            )
            continue
        try:
            manifest = contracts.load_manifest(path)
        except contracts.ContractError as error:
            issues.append(_issue("invalid_manifest", str(manifest_id), str(error)))
            continue
        if manifest.manifest_id != manifest_id:
            issues.append(
                _issue(
                    "manifest_identity_mismatch",
                    str(manifest_id),
                    "manifest content has a different identity",
                )
            )
            continue
        expected[manifest_id].update(
            {
                "sha256": item["sha256"],
                "manifest": manifest,
                "bundle_id": manifest.build.bundle_id,
                "bundle_sha256": manifest.build.sha256,
            }
        )
    for manifest_id in sorted(set(expected) - seen):
        issues.append(
            _issue(
                "missing_manifest",
                manifest_id,
                "accepted manifest is absent from the plan bundle",
            )
        )
    return expected, issues


def _check_builds(
    plan: dict[str, Any],
    expected_manifests: dict[str, dict[str, Any]],
    builds_dir: Path | None,
) -> list[dict[str, str]]:
    expected = {group["id"]: group for group in plan["buildGroups"]}
    if builds_dir is None or not builds_dir.is_dir():
        return [
            _issue(
                "missing_build_bundle",
                group_id,
                "accepted build group has no downloaded bundle",
            )
            for group_id in sorted(expected)
        ]
    issues: list[dict[str, str]] = []
    seen: set[str] = set()
    for metadata_path in sorted(builds_dir.rglob(build_bundle.METADATA_NAME)):
        try:
            verified = build_bundle.verify_bundle(metadata_path.parent)
        except build_bundle.BundleError as error:
            issues.append(
                _issue("invalid_build_bundle", str(metadata_path.parent), str(error))
            )
            continue
        if verified.bundle_id in seen:
            issues.append(
                _issue(
                    "duplicate_build_bundle",
                    verified.bundle_id,
                    "build identity was downloaded more than once",
                )
            )
            continue
        seen.add(verified.bundle_id)
        group = expected.get(verified.bundle_id)
        if group is None:
            issues.append(
                _issue(
                    "unexpected_build_bundle",
                    verified.bundle_id,
                    "bundle was not declared by the accepted plan",
                )
            )
        elif verified.request_sha256 != group["requestSha256"]:
            issues.append(
                _issue(
                    "build_request_mismatch",
                    verified.bundle_id,
                    "bundle request digest differs from the accepted group",
                )
            )
        else:
            content_digests = {
                item["bundle_sha256"]
                for item in expected_manifests.values()
                if item["bundle_id"] == verified.bundle_id
                and item["bundle_sha256"] is not None
            }
            if content_digests and content_digests != {verified.sha256}:
                issues.append(
                    _issue(
                        "build_content_mismatch",
                        verified.bundle_id,
                        "downloaded bundle differs from the manifest identity",
                    )
                )
    for group_id in sorted(set(expected) - seen):
        issues.append(
            _issue(
                "missing_build_bundle",
                group_id,
                "accepted build group has no downloaded bundle",
            )
        )
    return issues


def aggregate(
    accepted_plan: dict[str, Any],
    *,
    results_dir: Path,
    plan_bundle_path: Path | None = None,
    builds_dir: Path | None = None,
) -> AggregateReport:
    """Classify one immutable generation; discovery never hides infrastructure."""
    plan = _plan(accepted_plan)
    expected, issues = _expected_manifests(plan, plan_bundle_path)
    issues.extend(_check_builds(plan, expected, builds_dir))
    result_paths = (
        sorted(results_dir.rglob("runner_result.json")) if results_dir.is_dir() else []
    )
    results_by_manifest: dict[str, list[tuple[Path, dict[str, Any]]]] = {}
    for path in result_paths:
        try:
            document = _read_object(path)
        except AggregateError as error:
            issues.append(_issue("invalid_runner_result", str(path), str(error)))
            continue
        manifest_id = document.get("manifest_id")
        if not isinstance(manifest_id, str):
            issues.append(
                _issue(
                    "invalid_runner_result",
                    str(path),
                    "runner result has no manifest identity",
                )
            )
            continue
        results_by_manifest.setdefault(manifest_id, []).append((path, document))

    rows: list[dict[str, Any]] = []
    findings: dict[str, dict[str, Any]] = {}
    crashed_boots = 0
    stalled_boots = 0
    for manifest_id, manifest_expected in sorted(expected.items()):
        matches = results_by_manifest.pop(manifest_id, [])
        if not matches:
            issues.append(
                _issue(
                    "missing_runner_result",
                    manifest_id,
                    "no runner result was retained",
                )
            )
            rows.append({"manifest_id": manifest_id, "state": "missing"})
            continue
        if len(matches) > 1:
            issues.append(
                _issue(
                    "duplicate_runner_result",
                    manifest_id,
                    f"found {len(matches)} results for one manifest",
                )
            )
        path, result = matches[0]
        if result.get("schema_version") != 1 or result.get("lifecycle") not in (
            "completed",
            "failed",
            "cancelled",
        ):
            issues.append(
                _issue(
                    "invalid_runner_result",
                    manifest_id,
                    "runner result has an invalid version or lifecycle",
                )
            )
        if (
            manifest_expected["sha256"] is not None
            and result.get("manifest_sha256") != manifest_expected["sha256"]
        ):
            issues.append(
                _issue(
                    "result_identity_mismatch",
                    manifest_id,
                    "runner result names a different manifest digest",
                )
            )
        copied_manifest = path.parent / "manifest.json"
        if manifest_expected["sha256"] is not None and (
            not copied_manifest.is_file()
            or contracts.sha256_file(copied_manifest) != manifest_expected["sha256"]
        ):
            issues.append(
                _issue(
                    "result_manifest_mismatch",
                    manifest_id,
                    "retained manifest does not match the planned manifest",
                )
            )
        try:
            identity = _read_object(path.parent / "identity.json")
        except AggregateError as error:
            issues.append(_issue("missing_identity", manifest_id, str(error)))
        else:
            planned = manifest_expected["manifest"]
            if planned and identity != executor.identity(
                manifest_sha256=manifest_expected["sha256"], manifest=planned
            ):
                issues.append(
                    _issue(
                        "identity_sidecar_mismatch",
                        manifest_id,
                        "runner identity sidecar differs from the accepted manifest",
                    )
                )
        execution_value = result.get("execution")
        execution: dict[str, Any] = (
            execution_value if isinstance(execution_value, dict) else {}
        )
        discovery_value = result.get("discovery")
        discovery: dict[str, Any] = (
            discovery_value if isinstance(discovery_value, dict) else {}
        )
        health = execution.get("health", "infrastructure")
        lifecycle = result.get("lifecycle", "failed")
        if health != "healthy" or lifecycle != "completed":
            issues.append(
                _issue(
                    "runner_infrastructure",
                    manifest_id,
                    f"lifecycle={lifecycle}, health={health}, code={execution.get('code')}",
                )
            )
        campaign_value = result.get("campaign")
        campaign: dict[str, Any] = (
            campaign_value if isinstance(campaign_value, dict) else {}
        )
        summary_value = campaign.get("summary")
        summary: dict[str, Any] = (
            summary_value if isinstance(summary_value, dict) else {}
        )
        crashed = _count(summary.get("crashed_boots"))
        stalled = _count(summary.get("stalled_boots"))
        crashed_boots += crashed
        stalled_boots += stalled
        for finding in campaign.get("findings", []):
            if not isinstance(finding, dict):
                continue
            sig = str(finding.get("sig") or "unknown")
            entry = findings.setdefault(
                sig,
                {
                    "sig": sig,
                    "tier": finding.get("tier"),
                    "kind": finding.get("kind"),
                    "site": finding.get("site"),
                    "msg": finding.get("msg"),
                    "occurrences": 0,
                    "manifests": [],
                },
            )
            entry["occurrences"] += int(finding.get("occurrences", 1))
            if manifest_id not in entry["manifests"]:
                entry["manifests"].append(manifest_id)
        replay_value = result.get("replay")
        replay = replay_value.get("argv", []) if isinstance(replay_value, dict) else []
        rows.append(
            {
                "manifest_id": manifest_id,
                "bundle_id": manifest_expected["bundle_id"],
                "state": lifecycle,
                "health": health,
                "discovery": discovery.get("kind", "none"),
                "finding_count": discovery.get("finding_count", 0),
                "crashed_boots": crashed,
                "stalled_boots": stalled,
                "result": str(path),
                "replay": replay,
                "trace": campaign.get("trace", []),
            }
        )
    for manifest_id, matches in sorted(results_by_manifest.items()):
        for path, _document in matches:
            issues.append(
                _issue(
                    "unexpected_runner_result",
                    manifest_id,
                    f"unplanned result at {path}",
                )
            )

    partial = bool(issues)
    document = {
        "schema_version": 1,
        "plan_id": plan["id"],
        "batch_id": plan["batchId"],
        "ok": not partial,
        "partial": partial,
        "expected_manifests": len(expected),
        "received_results": sum(1 for row in rows if row["state"] != "missing"),
        "discovery": {
            "finding_count": sum(item["occurrences"] for item in findings.values()),
            "unique_findings": len(findings),
            "crashed_boots": crashed_boots,
            "stalled_boots": stalled_boots,
        },
        "infrastructure": {"issue_count": len(issues), "issues": issues},
        "results": rows,
        "findings": sorted(findings.values(), key=lambda item: item["sig"]),
    }
    return AggregateReport(document)


def _count(value: Any) -> int:
    return value if isinstance(value, int) and not isinstance(value, bool) else 0


def render_markdown(report: AggregateReport) -> str:
    document = report.document
    discovery = document["discovery"]
    found_something = bool(
        discovery["finding_count"]
        or discovery.get("crashed_boots")
        or discovery.get("stalled_boots")
    )
    if report.partial:
        icon = "⚠️"
    elif found_something:
        icon = "🐛"
    else:
        icon = "✅"
    lines = [
        f"## {icon} Nightmare batch `{document['batch_id']}`",
        "",
        f"Results: **{document['received_results']}/{document['expected_manifests']}** · "
        f"findings: **{discovery['finding_count']}** · "
        f"crashed boots: **{discovery.get('crashed_boots', 0)}** · "
        f"stalled boots: **{discovery.get('stalled_boots', 0)}** · "
        f"infrastructure issues: **{document['infrastructure']['issue_count']}**",
        "",
        "### Runner results",
        "",
    ]
    headers = [
        "Manifest",
        "Build",
        "Lifecycle",
        "Health",
        "Discovery",
        "Findings",
        "Crashed",
    ]
    rows = [
        [
            RP.md_code(row["manifest_id"]),
            RP.md_code(row.get("bundle_id", "-")),
            str(row["state"]),
            str(row.get("health", "-")),
            str(row.get("discovery", "-")),
            str(row.get("finding_count", 0)),
            str(row.get("crashed_boots", 0)),
        ]
        for row in document["results"]
    ]
    RP.md_table(lines.append, headers, rows)
    if document["findings"]:
        lines.extend(["", "### Findings", ""])
        for finding in document["findings"]:
            lines.append(
                f"- `{finding['sig']}` — {finding['kind']} at `{finding['site']}` "
                f"({finding['occurrences']} occurrence(s)): {finding['msg']}"
            )
    traced = [row for row in document["results"] if row.get("trace")]
    if traced:
        lines.extend(["", "### Progress traces", ""])
        for row in traced:
            trace = row["trace"]
            final = trace[-1]
            progress = final.get("cumulative_progress", final.get("progress", 0))
            at_ms = final.get("at_ms", 0)
            lines.append(
                f"- `{row['manifest_id']}` — {len(trace)} sample(s), "
                f"final progress **{progress}** at **{at_ms} ms**"
            )
    if document["infrastructure"]["issues"]:
        lines.extend(["", "### Infrastructure", ""])
        for issue in document["infrastructure"]["issues"]:
            lines.append(
                f"- **{issue['code']}** `{issue['subject']}` — {issue['message']}"
            )
    lines.extend(["", "### Replay", ""])
    for row in document["results"]:
        if row.get("replay"):
            command = " ".join(str(item) for item in row["replay"])
            lines.append(
                f"- `{row['manifest_id']}`: `{command}` with bundle `{row.get('bundle_id')}`"
            )
    return "\n".join(lines) + "\n"


def write(report: AggregateReport, *, json_path: Path, markdown_path: Path) -> None:
    json_path.write_text(json.dumps(report.document, indent=2) + "\n", encoding="utf-8")
    markdown_path.write_text(render_markdown(report), encoding="utf-8")
