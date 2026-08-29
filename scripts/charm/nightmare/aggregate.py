"""Completeness-first aggregation for manifest-driven Nightmare executions."""

import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from . import build_bundle, contracts


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
            "path": None,
        }
        for item in plan["manifests"]
    }
    issues: list[dict[str, str]] = []
    if plan_bundle_path is None or not plan_bundle_path.is_file():
        issues.append(
            {
                "code": "missing_plan_bundle",
                "subject": plan["id"],
                "message": "accepted plan was not materialized",
            }
        )
        return expected, issues

    index = _read_object(plan_bundle_path)
    if index.get("planId") != plan["id"]:
        issues.append(
            {
                "code": "plan_identity_mismatch",
                "subject": str(index.get("planId")),
                "message": "materialized plan ID does not match the accepted plan",
            }
        )
        return expected, issues
    seen: set[str] = set()
    for item in index.get("manifests", []):
        manifest_id = item.get("manifestId")
        if manifest_id in seen:
            issues.append(
                {
                    "code": "duplicate_manifest",
                    "subject": str(manifest_id),
                    "message": "manifest appears more than once in the plan bundle",
                }
            )
            continue
        seen.add(manifest_id)
        if manifest_id not in expected:
            issues.append(
                {
                    "code": "unexpected_manifest",
                    "subject": str(manifest_id),
                    "message": "plan bundle contains an unplanned manifest",
                }
            )
            continue
        try:
            path = _safe_relative(plan_bundle_path.parent, item["path"])
        except (KeyError, TypeError, AggregateError) as error:
            issues.append(
                {
                    "code": "invalid_manifest_path",
                    "subject": str(manifest_id),
                    "message": str(error),
                }
            )
            continue
        if not path.is_file() or contracts.sha256_file(path) != item.get("sha256"):
            issues.append(
                {
                    "code": "manifest_digest_mismatch",
                    "subject": str(manifest_id),
                    "message": "materialized manifest is missing or has the wrong digest",
                }
            )
            continue
        try:
            manifest = contracts.load_manifest(path)
        except contracts.ContractError as error:
            issues.append(
                {
                    "code": "invalid_manifest",
                    "subject": str(manifest_id),
                    "message": str(error),
                }
            )
            continue
        if manifest.manifest_id != manifest_id:
            issues.append(
                {
                    "code": "manifest_identity_mismatch",
                    "subject": str(manifest_id),
                    "message": "manifest content has a different identity",
                }
            )
            continue
        expected[manifest_id].update(
            {
                "sha256": item["sha256"],
                "path": path,
                "bundle_id": manifest.build.bundle_id,
                "bundle_sha256": manifest.build.sha256,
            }
        )
    for manifest_id in sorted(set(expected) - seen):
        issues.append(
            {
                "code": "missing_manifest",
                "subject": manifest_id,
                "message": "accepted manifest is absent from the plan bundle",
            }
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
            {
                "code": "missing_build_bundle",
                "subject": group_id,
                "message": "accepted build group has no downloaded bundle",
            }
            for group_id in sorted(expected)
        ]
    issues: list[dict[str, str]] = []
    seen: set[str] = set()
    for metadata_path in sorted(builds_dir.rglob(build_bundle.METADATA_NAME)):
        try:
            verified = build_bundle.verify_bundle(metadata_path.parent)
        except build_bundle.BundleError as error:
            issues.append(
                {
                    "code": "invalid_build_bundle",
                    "subject": str(metadata_path.parent),
                    "message": str(error),
                }
            )
            continue
        if verified.bundle_id in seen:
            issues.append(
                {
                    "code": "duplicate_build_bundle",
                    "subject": verified.bundle_id,
                    "message": "build identity was downloaded more than once",
                }
            )
            continue
        seen.add(verified.bundle_id)
        group = expected.get(verified.bundle_id)
        if group is None:
            issues.append(
                {
                    "code": "unexpected_build_bundle",
                    "subject": verified.bundle_id,
                    "message": "bundle was not declared by the accepted plan",
                }
            )
        elif verified.request_sha256 != group["requestSha256"]:
            issues.append(
                {
                    "code": "build_request_mismatch",
                    "subject": verified.bundle_id,
                    "message": "bundle request digest differs from the accepted group",
                }
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
                    {
                        "code": "build_content_mismatch",
                        "subject": verified.bundle_id,
                        "message": "downloaded bundle differs from the manifest identity",
                    }
                )
    for group_id in sorted(set(expected) - seen):
        issues.append(
            {
                "code": "missing_build_bundle",
                "subject": group_id,
                "message": "accepted build group has no downloaded bundle",
            }
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
            issues.append(
                {
                    "code": "invalid_runner_result",
                    "subject": str(path),
                    "message": str(error),
                }
            )
            continue
        manifest_id = document.get("manifest_id")
        if not isinstance(manifest_id, str):
            issues.append(
                {
                    "code": "invalid_runner_result",
                    "subject": str(path),
                    "message": "runner result has no manifest identity",
                }
            )
            continue
        results_by_manifest.setdefault(manifest_id, []).append((path, document))

    rows: list[dict[str, Any]] = []
    findings: dict[str, dict[str, Any]] = {}
    for manifest_id, manifest_expected in sorted(expected.items()):
        matches = results_by_manifest.pop(manifest_id, [])
        if not matches:
            issues.append(
                {
                    "code": "missing_runner_result",
                    "subject": manifest_id,
                    "message": "no runner result was retained",
                }
            )
            rows.append({"manifest_id": manifest_id, "state": "missing"})
            continue
        if len(matches) > 1:
            issues.append(
                {
                    "code": "duplicate_runner_result",
                    "subject": manifest_id,
                    "message": f"found {len(matches)} results for one manifest",
                }
            )
        path, result = matches[0]
        if result.get("schema_version") != 1 or result.get("lifecycle") not in (
            "completed",
            "failed",
            "cancelled",
        ):
            issues.append(
                {
                    "code": "invalid_runner_result",
                    "subject": manifest_id,
                    "message": "runner result has an invalid version or lifecycle",
                }
            )
        if (
            manifest_expected["sha256"] is not None
            and result.get("manifest_sha256") != manifest_expected["sha256"]
        ):
            issues.append(
                {
                    "code": "result_identity_mismatch",
                    "subject": manifest_id,
                    "message": "runner result names a different manifest digest",
                }
            )
        copied_manifest = path.parent / "manifest.json"
        if manifest_expected["sha256"] is not None and (
            not copied_manifest.is_file()
            or contracts.sha256_file(copied_manifest) != manifest_expected["sha256"]
        ):
            issues.append(
                {
                    "code": "result_manifest_mismatch",
                    "subject": manifest_id,
                    "message": "retained manifest does not match the planned manifest",
                }
            )
        identity_path = path.parent / "identity.json"
        try:
            identity = _read_object(identity_path)
        except AggregateError as error:
            issues.append(
                {
                    "code": "missing_identity",
                    "subject": manifest_id,
                    "message": str(error),
                }
            )
        else:
            manifest_path = manifest_expected["path"]
            planned_manifest = (
                contracts.load_manifest(manifest_path)
                if isinstance(manifest_path, Path)
                else None
            )
            expected_identity = (
                {
                    "manifest_id": manifest_id,
                    "manifest_sha256": manifest_expected["sha256"],
                    "source": planned_manifest.document["source"],
                    "suite": {
                        "id": planned_manifest.suite.id,
                        "sha256": planned_manifest.suite.sha256,
                    },
                    "build": planned_manifest.document["build"],
                }
                if planned_manifest is not None
                else None
            )
            if expected_identity is not None and any(
                identity.get(field) != value
                for field, value in expected_identity.items()
            ):
                issues.append(
                    {
                        "code": "identity_sidecar_mismatch",
                        "subject": manifest_id,
                        "message": "runner identity sidecar differs from the accepted manifest",
                    }
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
                {
                    "code": "runner_infrastructure",
                    "subject": manifest_id,
                    "message": f"lifecycle={lifecycle}, health={health}, code={execution.get('code')}",
                }
            )
        campaign_value = result.get("campaign")
        campaign: dict[str, Any] = (
            campaign_value if isinstance(campaign_value, dict) else {}
        )
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
                "result": str(path),
                "replay": replay,
                "trace": campaign.get("trace", []),
            }
        )
    for manifest_id, matches in sorted(results_by_manifest.items()):
        for path, _document in matches:
            issues.append(
                {
                    "code": "unexpected_runner_result",
                    "subject": manifest_id,
                    "message": f"unplanned result at {path}",
                }
            )

    partial = bool(issues)
    document = {
        "schema_version": 1,
        "plan_id": plan["id"],
        "batch_id": plan["batch"]["id"],
        "ok": not partial,
        "partial": partial,
        "expected_manifests": len(expected),
        "received_results": sum(1 for row in rows if row["state"] != "missing"),
        "discovery": {
            "finding_count": sum(item["occurrences"] for item in findings.values()),
            "unique_findings": len(findings),
        },
        "infrastructure": {"issue_count": len(issues), "issues": issues},
        "results": rows,
        "findings": sorted(findings.values(), key=lambda item: item["sig"]),
    }
    return AggregateReport(document)


def render_markdown(report: AggregateReport) -> str:
    document = report.document
    icon = "⚠️" if report.partial else "✅"
    lines = [
        f"## {icon} Nightmare batch `{document['batch_id']}`",
        "",
        f"Results: **{document['received_results']}/{document['expected_manifests']}** · "
        f"findings: **{document['discovery']['finding_count']}** · "
        f"infrastructure issues: **{document['infrastructure']['issue_count']}**",
        "",
        "### Runner results",
        "",
        "| Manifest | Build | Lifecycle | Health | Discovery | Findings |",
        "| --- | --- | --- | --- | --- | ---: |",
    ]
    for row in document["results"]:
        lines.append(
            f"| `{row['manifest_id']}` | `{row.get('bundle_id', '-')}` | "
            f"{row['state']} | {row.get('health', '-')} | {row.get('discovery', '-')} | "
            f"{row.get('finding_count', 0)} |"
        )
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


# ---------------------------------------------------------------------------
# Parity verification (merged from parity.py)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ParityVerdict:
    matches: bool
    differences: tuple[str, ...]
    findings_count: int
    traces_consistent: bool
    infrastructure_ok: bool

    def as_dict(self) -> dict[str, Any]:
        return {
            "matches": self.matches,
            "differences": list(self.differences),
            "findingsCount": self.findings_count,
            "tracesConsistent": self.traces_consistent,
            "infrastructureOk": self.infrastructure_ok,
        }


def normalize_signature(signature: str | dict[str, Any]) -> str:
    if isinstance(signature, str):
        return signature.strip().lower()
    if isinstance(signature, dict):
        lane = signature.get("lane", "")
        check = signature.get("check", "")
        return f"{lane}:{check}".strip().lower()
    return str(signature).strip().lower()


def verify_finding_parity(
    static_findings: list[dict[str, Any]],
    orchestrator_findings: list[dict[str, Any]],
) -> tuple[bool, list[str]]:
    diffs: list[str] = []
    static_sigs = {
        normalize_signature(f.get("signature", f.get("kind", "")))
        for f in static_findings
    }
    orch_sigs = {
        normalize_signature(f.get("signature", f.get("kind", "")))
        for f in orchestrator_findings
    }

    missing_in_orch = static_sigs - orch_sigs
    if missing_in_orch:
        diffs.append(f"findings missing in orchestrator: {sorted(missing_in_orch)}")

    missing_in_static = orch_sigs - static_sigs
    if missing_in_static:
        diffs.append(
            f"findings unexpected in orchestrator: {sorted(missing_in_static)}"
        )

    return len(diffs) == 0, diffs


def verify_trace_monotonicity(trace: list[dict[str, Any]]) -> bool:
    last_iterations = -1
    last_time = -1
    for point in trace:
        at_val = point.get("at_ms", point.get("at", 0))
        iterations = point.get("cumulative_progress", point.get("iterations", 0))
        if isinstance(at_val, (int, float)):
            if at_val < last_time:
                return False
            last_time = int(at_val)
        if isinstance(iterations, (int, float)):
            if iterations < last_iterations:
                return False
            last_iterations = int(iterations)
    return True


def check_execution_parity(
    static_report: dict[str, Any],
    orchestrator_report: dict[str, Any],
) -> ParityVerdict:
    diffs: list[str] = []

    # 1. Findings parity
    static_findings = static_report.get("findings", [])
    orch_findings = orchestrator_report.get("findings", [])
    _findings_match, finding_diffs = verify_finding_parity(
        static_findings, orch_findings
    )
    diffs.extend(finding_diffs)

    # 2. Trace monotonicity across results
    traces_ok = True
    for result in orchestrator_report.get("results", []):
        trace = result.get("trace", [])
        if not verify_trace_monotonicity(trace):
            traces_ok = False
            diffs.append(f"trace non-monotonic in manifest {result.get('manifest_id')}")

    # 3. Infrastructure health vs discovery
    orch_ok = orchestrator_report.get("ok", True)
    orch_partial = orchestrator_report.get("partial", False)
    if orch_partial and orch_ok:
        diffs.append("partial execution report marked ok=true")

    return ParityVerdict(
        matches=len(diffs) == 0,
        differences=tuple(diffs),
        findings_count=len(orch_findings),
        traces_consistent=traces_ok,
        infrastructure_ok=bool(orch_ok and not orch_partial),
    )
