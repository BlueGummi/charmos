"""The six-operation CI Studio interface over durable Nightmare authority."""

from __future__ import annotations

import copy
import re
from dataclasses import dataclass, replace
from datetime import UTC, datetime
from typing import Any

from ..nightmare import contracts
from . import domain, planner
from . import reconciler as R
from .coordinator import Coordinator

_FLEET_VERSION = re.compile(r"^fleet_r(?P<revision>[0-9]+)_[0-9a-f]{16}$")


class OperatorError(ValueError):
    """An invalid call at the operator interface."""


@dataclass(frozen=True)
class Audit:
    actor_id: str
    actor_login: str
    request_id: str

    def __post_init__(self) -> None:
        if not self.actor_id or not self.actor_login or not self.request_id:
            raise OperatorError("actor and request audit metadata are required")


@dataclass(frozen=True)
class FleetQuery:
    repository_id: str
    ref: str
    starts_at: datetime
    ends_at: datetime

    @classmethod
    def parse(cls, document: dict[str, Any]) -> FleetQuery:
        try:
            query = cls(
                repository_id=str(document["repositoryId"]),
                ref=str(document["ref"]),
                starts_at=_parse_instant(document["startsAt"], "startsAt"),
                ends_at=_parse_instant(document["endsAt"], "endsAt"),
            )
        except KeyError as error:
            raise OperatorError(f"missing fleet query field {error.args[0]}") from None
        if query.ends_at <= query.starts_at:
            raise OperatorError("fleet window must end after it starts")
        if query.ends_at.timestamp() - query.starts_at.timestamp() > 8 * 86_400:
            raise OperatorError("fleet window may not exceed eight days")
        return query


@dataclass(frozen=True)
class OperatorConfig:
    repository_id: str
    repository: str
    owner: str
    name: str
    default_ref: str
    refs: tuple[str, ...]
    source_commit: str
    runner_image: str
    runner_capacity: int = 10

    def __post_init__(self) -> None:
        if self.default_ref not in self.refs:
            raise OperatorError("default_ref must be present in refs")
        if not re.fullmatch(r"[0-9a-f]{40}", self.source_commit):
            raise OperatorError("source_commit must be a full 40-hex commit")
        if self.runner_capacity < 1 or self.runner_capacity > 256:
            raise OperatorError("runner_capacity must be in [1, 256]")


class Operator:
    """Own admission and mutation semantics behind CI Studio's six calls."""

    def __init__(
        self,
        coordinator: Coordinator,
        planner_: planner.Planner,
        config: OperatorConfig,
    ) -> None:
        self.coordinator = coordinator
        self.planner = planner_
        self.config = config

    def get_fleet_window(self, query: FleetQuery) -> dict[str, Any]:
        self._validate_query(query)
        for _attempt in range(self.coordinator.retries):
            envelope = self.coordinator.store.read()
            snapshot = self._snapshot(envelope.state, query, envelope.observed_at)
            recorded = self.coordinator.apply(
                R.RecordSnapshot(snapshot, envelope.state.authority_revision)
            )
            if recorded.result.get("kind") == "accepted":
                return snapshot
        raise RuntimeError("fleet changed during every snapshot retry")

    def get_batch(self, batch_id: str) -> dict[str, Any] | None:
        return self.coordinator.store.read().state.batches.get(batch_id)

    def validate_batch(self, body: dict[str, Any], audit: Audit) -> dict[str, Any]:
        command = self._command("validate_batch", body, audit)
        replay = self._append(command, audit, wake=False)
        if _terminal(replay):
            return replay

        envelope = self.coordinator.store.read()
        base_version = command["payload"]["base_snapshot_version"]
        snapshot = envelope.state.snapshots.get(base_version)
        if (
            snapshot is None
            or _revision(base_version) != envelope.state.authority_revision
        ):
            result = self._stale_result(snapshot, envelope)
            return self.coordinator.apply(
                R.RecordPlan(command["command_id"], result, None)
            ).result

        result = planner.render_result(
            self.planner.plan(
                command,
                snapshot,
                source=domain.Source(self.config.repository, self.config.source_commit),
                runner_image=self.config.runner_image,
                now=envelope.observed_at,
                ownership="ad-hoc",
            )
        )
        recorded = self.coordinator.apply(
            R.RecordPlan(
                command["command_id"],
                result,
                snapshot,
                expected_authority_revision=envelope.state.authority_revision,
            )
        )
        if recorded.result.get("code") == "snapshot_changed":
            latest = self.coordinator.store.read()
            return self._stale_result(snapshot, latest)
        return recorded.result

    def submit_batch(self, body: dict[str, Any], audit: Audit) -> dict[str, Any]:
        command = self._command("submit_batch", body, audit)
        replay = self._append(command, audit, wake=False)
        if _terminal(replay):
            return replay

        envelope = self.coordinator.store.read()
        payload = command["payload"]
        stored = envelope.state.plans.get(payload["plan_id"])
        plan = stored.get("plan") if isinstance(stored, dict) else None
        if not isinstance(plan, dict):
            return self._reject_command(
                command["command_id"],
                "submission_rejected",
                "The accepted plan does not exist.",
            )
        if plan.get("version") != payload["plan_version"]:
            return self._reject_command(
                command["command_id"],
                "submission_rejected",
                "The accepted plan version does not match.",
            )
        current_version = self._current_version(plan["baseSnapshotVersion"], envelope)
        if (
            plan.get("baseSnapshotVersion") != payload["base_snapshot_version"]
            or _revision(plan["baseSnapshotVersion"])
            != envelope.state.authority_revision
        ):
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "stale",
                    "code": "snapshot_changed",
                    "message": "Fleet state changed. Validate the preserved TOML again.",
                    "currentVersion": current_version,
                },
            )
        if _parse_instant(plan["expiresAt"], "expiresAt") <= envelope.observed_at:
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "stale",
                    "code": "plan_expired",
                    "message": "The accepted plan expired. Validate the preserved TOML again.",
                    "currentVersion": current_version,
                },
            )

        batch = copy.deepcopy(plan["batch"])
        batch["lifecycle"] = "queued"
        batch["version"] = _id(
            "batchv",
            {"plan": plan["id"], "revision": envelope.state.authority_revision + 1},
        )
        batch["updatedAt"] = _instant(envelope.observed_at)
        snapshot = self._mutation_snapshot(envelope, plan["baseSnapshotVersion"], batch)
        result = {"kind": "accepted", "snapshot": snapshot, "batch": batch}
        committed = self.coordinator.apply(
            R.CommitBatch(
                command["command_id"],
                plan["id"],
                batch,
                snapshot,
                result,
                envelope.state.authority_revision,
                wake=True,
            )
        )
        if committed.result.get("kind") == "conflict":
            latest = self.coordinator.store.read()
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "stale",
                    "code": "snapshot_changed",
                    "message": "Fleet state changed. Validate the preserved TOML again.",
                    "currentVersion": self._current_version(
                        plan["baseSnapshotVersion"], latest
                    ),
                },
            )
        return committed.result

    def draft_tail_update(self, body: dict[str, Any], audit: Audit) -> dict[str, Any]:
        command = self._command("draft_tail_update", body, audit)
        replay = self._append(command, audit, wake=False)
        if _terminal(replay):
            return replay
        envelope = self.coordinator.store.read()
        payload = command["payload"]
        batch = envelope.state.batches.get(payload["batch_id"])
        if (
            batch is None
            or batch.get("ownership") != "ad-hoc"
            or batch.get("residualTail") is None
        ):
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "rejected",
                    "code": "immutable",
                    "message": "Only the residual tail of an ad-hoc batch is mutable.",
                    "diagnostics": [],
                },
            )
        if batch["version"] != payload["base_batch_version"]:
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "conflict",
                    "code": "batch_changed",
                    "message": "The batch changed. Refresh it before drafting again.",
                    "currentVersion": batch["version"],
                },
            )
        mutable = set(batch["residualTail"]["mutableTaskIds"])
        cancelled = set(payload["cancelled_task_ids"])
        if not cancelled or not cancelled <= mutable:
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "rejected",
                    "code": "no_tail_capacity",
                    "message": "The draft may cancel only unstarted residual-tail tasks.",
                    "diagnostics": [
                        {
                            "field": "cancelledTaskIds",
                            "message": "contains a started, unknown, or already immutable task",
                        }
                    ],
                },
            )

        preview = copy.deepcopy(batch)
        for task in preview["tasks"]:
            if task["id"] in cancelled:
                task["cancelled"] = True
        remaining = [
            item
            for item in preview["residualTail"]["mutableTaskIds"]
            if item not in cancelled
        ]
        preview["residualTail"]["mutableTaskIds"] = remaining
        if not remaining:
            preview["residualTail"] = None
        preview["version"] = _id(
            "batchv", {"command": command, "batch": batch["version"]}
        )
        preview["updatedAt"] = _instant(envelope.observed_at)
        plan_id = _id("tailplan", {"command": command, "batch": batch})
        plan_version = _id("tailv", {"plan": plan_id, "preview": preview})
        tail_plan = {
            "id": plan_id,
            "version": plan_version,
            "batchId": batch["id"],
            "baseBatchVersion": batch["version"],
            "batch": preview,
        }
        result = {"kind": "accepted", "plan": tail_plan}
        execution_plan = self._tail_execution_plan(envelope.state, batch, preview)
        stored = {
            "kind": "accepted",
            "plan": tail_plan,
            "executionPlanId": execution_plan["plan"]["id"] if execution_plan else None,
        }
        return self.coordinator.apply(
            R.RecordPlan(
                command["command_id"],
                result,
                None,
                expected_authority_revision=envelope.state.authority_revision,
                stored_plan=stored,
                related_plans=(
                    {execution_plan["plan"]["id"]: execution_plan}
                    if execution_plan
                    else None
                ),
            )
        ).result

    def commit_tail_update(self, body: dict[str, Any], audit: Audit) -> dict[str, Any]:
        command = self._command("commit_tail_update", body, audit)
        replay = self._append(command, audit, wake=False)
        if _terminal(replay):
            return replay
        envelope = self.coordinator.store.read()
        payload = command["payload"]
        stored = envelope.state.plans.get(payload["plan_id"])
        plan = stored.get("plan") if isinstance(stored, dict) else None
        if not isinstance(plan, dict) or plan.get("version") != payload["plan_version"]:
            return self._reject_command(
                command["command_id"],
                "commit_rejected",
                "The accepted tail plan does not exist.",
            )
        batch = envelope.state.batches.get(plan["batchId"])
        if batch is None or batch["version"] != payload["base_batch_version"]:
            current = batch["version"] if batch else payload["base_batch_version"]
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "conflict",
                    "code": "batch_changed",
                    "message": "The batch changed. Draft the tail update again.",
                    "currentVersion": current,
                },
            )
        preview = copy.deepcopy(plan["batch"])
        base_snapshot = self._latest_snapshot_for_batch(envelope.state, batch)
        snapshot = self._mutation_snapshot(envelope, base_snapshot["version"], preview)
        result = {"kind": "accepted", "snapshot": snapshot, "batch": preview}
        execution_plan_id = (
            stored.get("executionPlanId") if isinstance(stored, dict) else None
        )
        committed = self.coordinator.apply(
            R.CommitBatch(
                command["command_id"],
                execution_plan_id or plan["id"],
                preview,
                snapshot,
                result,
                envelope.state.authority_revision,
                expected_batch_version=batch["version"],
                wake=True,
            )
        )
        if committed.result.get("kind") == "conflict":
            latest = self.coordinator.store.read().state.batches.get(batch["id"])
            return self._resolve_rejection(
                command["command_id"],
                {
                    "kind": "conflict",
                    "code": "batch_changed",
                    "message": "The batch changed. Draft the tail update again.",
                    "currentVersion": latest["version"] if latest else batch["version"],
                },
            )
        return committed.result

    def _append(
        self, command: dict[str, Any], audit: Audit, *, wake: bool
    ) -> dict[str, Any]:
        return self.coordinator.apply(
            R.SubmitCommand(command, request_id=audit.request_id, wake=wake)
        ).result

    def _command(
        self, operation: str, body: dict[str, Any], audit: Audit
    ) -> dict[str, Any]:
        key = body.get("idempotencyKey")
        if not isinstance(key, str) or len(key) < 8 or len(key) > 128:
            raise OperatorError("idempotencyKey must contain 8 to 128 characters")
        payload: dict[str, Any]
        if operation == "validate_batch":
            definition = body.get("definition")
            if not isinstance(definition, dict):
                raise OperatorError("definition must be an object")
            payload = {
                "definition": {
                    "name": definition.get("name"),
                    "start_utc": definition.get("startUtc"),
                    "window_hours": definition.get("windowHours"),
                    "runners": definition.get("runners"),
                    "color": definition.get("color"),
                    "tests": definition.get("tests"),
                },
                "definition_toml": body.get("definitionToml"),
                "base_snapshot_version": body.get("baseSnapshotVersion"),
            }
        elif operation == "submit_batch":
            payload = {
                "plan_id": body.get("planId"),
                "plan_version": body.get("planVersion"),
                "base_snapshot_version": body.get("baseSnapshotVersion"),
            }
        elif operation == "draft_tail_update":
            payload = {
                "batch_id": body.get("batchId"),
                "base_batch_version": body.get("baseBatchVersion"),
                "cancelled_task_ids": body.get("cancelledTaskIds"),
            }
        elif operation == "commit_tail_update":
            payload = {
                "plan_id": body.get("planId"),
                "plan_version": body.get("planVersion"),
                "base_batch_version": body.get("baseBatchVersion"),
            }
        else:  # pragma: no cover - private dispatch is exhaustive
            raise OperatorError(f"unknown operation {operation}")
        digest = contracts.sha256_json({"operation": operation, "idempotency_key": key})
        return {
            "schema_version": 1,
            "command_id": f"command_{digest[:20]}",
            "operation": operation,
            "idempotency_key": key,
            "actor": {"id": audit.actor_id, "login": audit.actor_login},
            "repository": {
                "id": self.config.repository,
                "ref": self.config.default_ref,
            },
            "payload": payload,
        }

    def _snapshot(
        self,
        state: Any,
        query: FleetQuery,
        now: datetime,
        *,
        revision: int | None = None,
    ) -> dict[str, Any]:
        revision = state.authority_revision if revision is None else revision
        batches = [
            copy.deepcopy(batch)
            for batch in state.batches.values()
            if _overlaps(batch["lease"], query.starts_at, query.ends_at)
        ]
        batches.sort(key=lambda batch: (batch["lease"]["startsAt"], batch["id"]))
        version = _fleet_id(revision, query)
        return {
            "schemaVersion": 1,
            "version": version,
            "evaluatedAt": _instant(now),
            "window": {
                "startsAt": _instant(query.starts_at),
                "endsAt": _instant(query.ends_at),
            },
            "repository": {
                "id": self.config.repository_id,
                "owner": self.config.owner,
                "name": self.config.name,
                "ref": query.ref,
                "refs": list(self.config.refs),
            },
            "runners": [
                {
                    "id": f"runner_{number}",
                    "number": number,
                    "state": "idle",
                    "label": f"Runner {number}",
                }
                for number in range(1, self.config.runner_capacity + 1)
            ],
            "batches": batches,
            "partial": any(
                attempt.status == "recovery_required"
                for attempt in state.attempts.values()
            ),
        }

    def _mutation_snapshot(
        self, envelope: Any, base_version: str, batch: dict[str, Any]
    ) -> dict[str, Any]:
        base = envelope.state.snapshots.get(base_version)
        if base is None:
            base = self._latest_snapshot_for_batch(envelope.state, batch)
        query = FleetQuery.parse(
            {
                "repositoryId": base["repository"]["id"],
                "ref": base["repository"]["ref"],
                "startsAt": base["window"]["startsAt"],
                "endsAt": base["window"]["endsAt"],
            }
        )
        state = replace(
            envelope.state,
            batches={**envelope.state.batches, batch["id"]: batch},
        )
        return self._snapshot(
            state,
            query,
            envelope.observed_at,
            revision=envelope.state.authority_revision + 1,
        )

    def _latest_snapshot_for_batch(
        self, state: Any, batch: dict[str, Any]
    ) -> dict[str, Any]:
        candidates = [
            snapshot
            for snapshot in state.snapshots.values()
            if snapshot["window"]["startsAt"] <= batch["lease"]["startsAt"]
            and snapshot["window"]["endsAt"] >= batch["lease"]["endsAt"]
        ]
        if candidates:
            return max(
                candidates,
                key=lambda item: (_revision(item["version"]), item["evaluatedAt"]),
            )
        query = FleetQuery(
            self.config.repository_id,
            self.config.default_ref,
            _parse_instant(batch["lease"]["startsAt"], "lease.startsAt"),
            _parse_instant(batch["lease"]["endsAt"], "lease.endsAt"),
        )
        return self._snapshot(state, query, datetime.now(UTC))

    def _tail_execution_plan(
        self, state: Any, current: dict[str, Any], preview: dict[str, Any]
    ) -> dict[str, Any] | None:
        source_plan_id = state.batch_plans.get(current["id"])
        stored = state.plans.get(source_plan_id) if source_plan_id else None
        accepted = copy.deepcopy(stored) if isinstance(stored, dict) else None
        plan = accepted.get("plan") if isinstance(accepted, dict) else None
        if not isinstance(plan, dict):
            return None
        retained = {task["id"] for task in preview["tasks"] if not task["cancelled"]}
        plan["batch"] = copy.deepcopy(preview)
        plan["manifests"] = [
            item for item in plan.get("manifests", []) if item["taskId"] in retained
        ]
        plan["buildGroups"] = [
            {
                **group,
                "taskIds": [item for item in group["taskIds"] if item in retained],
            }
            for group in plan.get("buildGroups", [])
            if any(item in retained for item in group["taskIds"])
        ]
        plan["id"] = _id("plan", {"tail": preview, "source": source_plan_id})
        plan["version"] = _id("planv", plan)
        return {"kind": "accepted", "plan": plan}

    def _validate_query(self, query: FleetQuery) -> None:
        if query.repository_id != self.config.repository_id:
            raise OperatorError("unknown repository")
        if query.ref not in self.config.refs:
            raise OperatorError("unknown repository ref")

    def _stale_result(
        self, base: dict[str, Any] | None, envelope: Any
    ) -> dict[str, Any]:
        current = self._current_version(base["version"] if base else "", envelope)
        return {
            "kind": "stale",
            "code": "snapshot_changed",
            "message": "Fleet state changed. Validate this definition again.",
            "currentVersion": current,
        }

    def _current_version(self, base_version: str, envelope: Any) -> str:
        base = envelope.state.snapshots.get(base_version)
        if base is None:
            candidates = list(envelope.state.snapshots.values())
            if candidates:
                return max(
                    candidates,
                    key=lambda item: (
                        _revision(item["version"]),
                        item["evaluatedAt"],
                    ),
                )["version"]
            return f"fleet_r{envelope.state.authority_revision}_{'0' * 16}"
        query = FleetQuery.parse(
            {
                "repositoryId": base["repository"]["id"],
                "ref": base["repository"]["ref"],
                "startsAt": base["window"]["startsAt"],
                "endsAt": base["window"]["endsAt"],
            }
        )
        return _fleet_id(envelope.state.authority_revision, query)

    def _reject_command(
        self, command_id: str, code: str, message: str
    ) -> dict[str, Any]:
        return self._resolve_rejection(
            command_id, {"kind": "rejected", "code": code, "message": message}
        )

    def _resolve_rejection(
        self, command_id: str, result: dict[str, Any]
    ) -> dict[str, Any]:
        return self.coordinator.apply(R.RecordPlan(command_id, result, None)).result


def _terminal(result: dict[str, Any]) -> bool:
    return result.get("status") not in ("queued", None) or any(
        key in result for key in ("plan", "snapshot", "currentVersion", "message")
    )


def _revision(version: str) -> int:
    match = _FLEET_VERSION.fullmatch(version)
    return int(match.group("revision")) if match else -1


def _fleet_id(revision: int, query: FleetQuery) -> str:
    digest = contracts.sha256_json(
        {
            "repository_id": query.repository_id,
            "ref": query.ref,
            "starts_at": _instant(query.starts_at),
            "ends_at": _instant(query.ends_at),
        }
    )
    return f"fleet_r{revision}_{digest[:16]}"


def _id(prefix: str, value: Any) -> str:
    return f"{prefix}_{contracts.sha256_json(value)[:20]}"


def _instant(value: datetime) -> str:
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _parse_instant(value: Any, field: str) -> datetime:
    if not isinstance(value, str):
        raise OperatorError(f"{field} must be an ISO-8601 instant")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        raise OperatorError(f"{field} must be an ISO-8601 instant") from None
    if parsed.tzinfo is None:
        raise OperatorError(f"{field} must include a UTC offset")
    return parsed.astimezone(UTC)


def _overlaps(lease: dict[str, Any], starts_at: datetime, ends_at: datetime) -> bool:
    return (
        _parse_instant(lease["startsAt"], "lease.startsAt") < ends_at
        and _parse_instant(lease["endsAt"], "lease.endsAt") > starts_at
    )
