"""Durable preparation and completion for one Actions execution generation."""

from __future__ import annotations

import copy
from dataclasses import dataclass
from datetime import timedelta
from typing import Any

from ..nightmare import contracts
from . import reconciler as R
from .coordinator import Coordinator
from .operator import Operator, _instant, _parse_instant


class RuntimeError(ValueError):
    """The durable wake cannot be turned into the requested execution."""


@dataclass(frozen=True)
class PreparedExecution:
    attempt_id: str
    command: dict[str, Any]
    plan: dict[str, Any]
    snapshot: dict[str, Any]


class ExecutionRuntime:
    """Connect a durable wake to immutable manifests and retained lifecycle."""

    def __init__(self, operator: Operator) -> None:
        self.operator = operator
        self.coordinator: Coordinator = operator.coordinator

    def prepare(
        self,
        *,
        wake_id: str,
        command_ref: str,
        owner: str,
        lease_ms: int = 30 * 60 * 1000,
    ) -> PreparedExecution:
        acquired = self.coordinator.apply(
            R.AcquireCoordinator(wake_id, owner, lease_ms)
        )
        if acquired.result.get("kind") != "accepted":
            raise RuntimeError(
                f"cannot acquire coordinator lease: {acquired.result.get('code')}"
            )
        state = self.coordinator.store.read().state
        command = state.commands.get(command_ref)
        if command is None or command.status not in ("queued", "applied"):
            raise RuntimeError("wake command is absent or is not executable")
        operation = command.document.get("operation")
        requested_plan_id = command.document.get("payload", {}).get("plan_id")
        if operation == "submit_batch":
            plan_id = requested_plan_id
        elif operation == "commit_tail_update":
            tail = state.plans.get(requested_plan_id, {})
            plan_id = tail.get("executionPlanId")
        else:
            raise RuntimeError(f"operation {operation!r} does not launch execution")
        stored = state.plans.get(plan_id, {})
        plan = stored.get("plan") if isinstance(stored, dict) else None
        if not isinstance(plan, dict):
            raise RuntimeError("command does not reference an immutable accepted plan")
        batch = state.batches.get(plan["batch"]["id"])
        if batch is None:
            raise RuntimeError("accepted plan has no submitted batch")
        manifest_ids = tuple(item["manifestId"] for item in plan["manifests"])
        if not manifest_ids:
            raise RuntimeError("accepted plan has no executable manifests")
        attempt_id = (
            "attempt_"
            + contracts.sha256_json(
                {
                    "wake_id": wake_id,
                    "plan_id": plan["id"],
                    "generation": command.generation,
                }
            )[:20]
        )
        registered = self.coordinator.apply(
            R.RegisterAttempt(attempt_id, plan["id"], manifest_ids)
        )
        if registered.result.get("kind") not in ("accepted",):
            raise RuntimeError(
                f"cannot register attempt: {registered.result.get('code')}"
            )
        started = self.coordinator.apply(R.StartExecution(command_ref, owner))
        if started.result.get("kind") != "accepted":
            raise RuntimeError(f"cannot start execution: {started.result.get('code')}")
        snapshot = self.operator._latest_snapshot_for_batch(state, batch)
        return PreparedExecution(
            attempt_id,
            command.document,
            {"kind": "accepted", "plan": plan},
            snapshot,
        )

    def finish(
        self,
        *,
        attempt_id: str,
        report: dict[str, Any],
        result_digests: dict[str, str],
        owner: str,
    ) -> dict[str, Any]:
        envelope = self.coordinator.store.read()
        attempt = envelope.state.attempts.get(attempt_id)
        if attempt is None:
            raise RuntimeError("unknown attempt")
        stored = envelope.state.plans.get(attempt.plan_id, {})
        plan = stored.get("plan") if isinstance(stored, dict) else None
        if not isinstance(plan, dict):
            raise RuntimeError("attempt has no retained accepted plan")
        if (
            report.get("plan_id") != plan["id"]
            or report.get("batch_id") != plan["batch"]["id"]
        ):
            raise RuntimeError("aggregate identity does not match the durable attempt")
        for manifest_id, digest in sorted(result_digests.items()):
            recorded = self.coordinator.apply(
                R.RecordResult(attempt_id, manifest_id, digest)
            )
            if recorded.result.get("kind") not in ("accepted",):
                raise RuntimeError(
                    f"cannot retain result {manifest_id}: {recorded.result.get('code')}"
                )

        envelope = self.coordinator.store.read()
        batch = envelope.state.batches.get(plan["batch"]["id"])
        if batch is None:
            raise RuntimeError("attempt batch is not retained")
        updated = _batch_from_report(batch, plan, report, envelope.observed_at)
        base = self.operator._latest_snapshot_for_batch(envelope.state, batch)
        snapshot = self.operator._mutation_snapshot(envelope, base["version"], updated)
        finalized = self.coordinator.apply(
            R.FinalizeBatch(
                attempt_id,
                updated,
                snapshot,
                envelope.state.authority_revision,
                batch["version"],
                bool(report.get("partial")),
            )
        )
        if finalized.result.get("kind") != "accepted":
            raise RuntimeError(f"cannot finalize batch: {finalized.result.get('code')}")
        self.coordinator.apply(R.CompleteDrain(owner))
        self.coordinator.apply(R.Tick(idle_grace_ms=0))
        return {**finalized.result, "snapshot": snapshot, "batch": updated}


def _batch_from_report(
    batch: dict[str, Any],
    plan: dict[str, Any],
    report: dict[str, Any],
    now: Any,
) -> dict[str, Any]:
    updated = copy.deepcopy(batch)
    partial = bool(report.get("partial"))
    updated["lifecycle"] = "failed" if partial else "completed"
    updated["health"] = "failed" if partial else "healthy"
    updated["residualTail"] = None
    updated["updatedAt"] = _instant(now)
    updated["version"] = (
        "batchv_"
        + contracts.sha256_json(
            {"batch": batch["version"], "report": report, "at": _instant(now)}
        )[:20]
    )
    task_for_manifest = {
        item["manifestId"]: item["taskId"] for item in plan["manifests"]
    }
    rows = {
        task_for_manifest[row["manifest_id"]]: row
        for row in report.get("results", [])
        if row.get("manifest_id") in task_for_manifest
    }
    started: list[str] = []
    for task in updated["tasks"]:
        if task.get("cancelled"):
            continue
        row = rows.get(task["id"])
        task["started"] = True
        task["slice"]["immutable"] = True
        started.append(task["id"])
        if (
            row is None
            or row.get("health") != "healthy"
            or row.get("state") != "completed"
        ):
            task["state"] = "crashed"
            task["health"] = "failed"
        elif row.get("discovery") not in (None, "none"):
            task["state"] = "warning"
            task["health"] = "warning"
        else:
            task["state"] = "passed"
            task["health"] = "healthy"
        start = _parse_instant(task["slice"]["startsAt"], "slice.startsAt")
        task["slice"]["trace"] = [
            {
                "at": _instant(
                    start + timedelta(milliseconds=int(sample.get("at_ms", 0)))
                ),
                "iterations": max(
                    0,
                    int(sample.get("cumulative_progress", sample.get("progress", 0))),
                ),
            }
            for sample in (row or {}).get("trace", [])
        ]
    updated["startedPrefixTaskIds"] = started
    return updated
