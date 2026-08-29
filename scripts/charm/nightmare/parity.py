"""Parity verification between static hunt fallback and dynamic manifest orchestrator."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


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
