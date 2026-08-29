"""Deterministic admission and placement behind one planning interface."""

import json
import math
import re
import tomllib
from collections import Counter, defaultdict
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from . import contracts, domain
from . import suite as suite_model

PLAN_TTL = timedelta(minutes=10)
MAX_MANIFESTS = 64
_COMMIT = re.compile(r"^[0-9a-f]{40}$")
_IMAGE = re.compile(r"^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$")
_REPOSITORY = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
_COLOR = re.compile(r"^#[0-9A-Fa-f]{6}$")


def _instant(value: datetime) -> str:
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _parse_instant(value: Any, field: str) -> datetime:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be an ISO-8601 string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        raise ValueError(f"{field} must be an ISO-8601 instant") from None
    if parsed.tzinfo is None:
        raise ValueError(f"{field} must include a UTC offset")
    return parsed.astimezone(UTC)


def _definition(payload: dict[str, Any]) -> domain.BatchDefinition:
    raw = payload["definition"]
    if not isinstance(raw, dict):
        raise ValueError("definition must be an object")
    name = raw["name"]
    window_hours = raw["window_hours"]
    runners = raw["runners"]
    color = raw["color"]
    tests = raw["tests"]
    if not isinstance(name, str) or not name or len(name) > 128:
        raise ValueError("definition.name must be a non-empty string")
    if (
        isinstance(window_hours, bool)
        or not isinstance(window_hours, (int, float))
        or not math.isfinite(window_hours)
        or window_hours <= 0
        or window_hours > 168
    ):
        raise ValueError("definition.window_hours must be in (0, 168]")
    if type(runners) is not int or runners < 1 or runners > 256:
        raise ValueError("definition.runners must be an integer in [1, 256]")
    if not isinstance(color, str) or not _COLOR.fullmatch(color):
        raise ValueError("definition.color must be a six-digit hex color")
    if (
        not isinstance(tests, list)
        or not tests
        or not all(isinstance(item, str) and item for item in tests)
    ):
        raise ValueError("definition.tests must be a non-empty string array")
    return domain.BatchDefinition(
        name=name,
        start=_parse_instant(raw["start_utc"], "definition.start_utc"),
        window_ms=int(window_hours * 3_600_000),
        runners=runners,
        color=color,
        tests=tuple(tests),
    )


def _toml_definition(source: str) -> dict[str, Any]:
    try:
        root = tomllib.loads(source)
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f"definition_toml is invalid: {error}") from None
    batch = root.get("batch")
    if not isinstance(batch, dict):
        raise ValueError("definition_toml must contain [batch]")
    return batch


def _definition_wire(definition: domain.BatchDefinition) -> dict[str, Any]:
    return {
        "name": definition.name,
        "startUtc": _instant(definition.start),
        "windowHours": definition.window_ms / 3_600_000,
        "runners": definition.runners,
        "color": definition.color,
        "tests": list(definition.tests),
    }


def _definition_toml_shape(definition: domain.BatchDefinition) -> dict[str, Any]:
    return {
        "name": definition.name,
        "start_utc": _instant(definition.start),
        "window_hours": definition.window_ms / 3_600_000,
        "runners": definition.runners,
        "color": definition.color,
        "tests": list(definition.tests),
    }


def _snapshot(document: dict[str, Any]) -> domain.FleetSnapshot:
    if document.get("schemaVersion") != 1:
        raise ValueError("snapshot.schemaVersion must be 1")
    window = document["window"]
    starts_at = _parse_instant(window["startsAt"], "window.startsAt")
    ends_at = _parse_instant(window["endsAt"], "window.endsAt")
    if ends_at <= starts_at:
        raise ValueError("snapshot window must end after it starts")
    runners = tuple(
        domain.Runner(item["id"], item["number"], item["state"], item["label"])
        for item in sorted(document["runners"], key=lambda item: item["number"])
    )
    if not runners:
        raise ValueError("snapshot must contain at least one runner")
    if any(
        not isinstance(runner.id, str)
        or type(runner.number) is not int
        or runner.number < 1
        or runner.state not in ("idle", "running", "warning", "failed")
        or not isinstance(runner.label, str)
        or not runner.label
        for runner in runners
    ):
        raise ValueError("snapshot contains an invalid runner")
    if len({runner.id for runner in runners}) != len(runners) or len(
        {runner.number for runner in runners}
    ) != len(runners):
        raise ValueError("snapshot runner IDs and numbers must be unique")
    runner_ids = {runner.id for runner in runners}
    occupied = []
    for batch in document.get("batches", []):
        lease = batch["lease"]
        lease_runner_ids = lease["runnerIds"]
        if (
            not isinstance(lease_runner_ids, list)
            or not lease_runner_ids
            or not all(isinstance(item, str) for item in lease_runner_ids)
            or len(set(lease_runner_ids)) != len(lease_runner_ids)
            or not set(lease_runner_ids) <= runner_ids
        ):
            raise ValueError("snapshot contains an invalid runner lease")
        lease_start = _parse_instant(lease["startsAt"], "batch.lease.startsAt")
        lease_end = _parse_instant(lease["endsAt"], "batch.lease.endsAt")
        if lease_end <= lease_start:
            raise ValueError("snapshot lease must end after it starts")
        occupied.append(
            domain.OccupiedLease(
                frozenset(lease_runner_ids),
                lease_start,
                lease_end,
            )
        )
    version = document["version"]
    if not isinstance(version, str) or not version:
        raise ValueError("snapshot.version must be a non-empty string")
    return domain.FleetSnapshot(
        version=version,
        starts_at=starts_at,
        ends_at=ends_at,
        runners=runners,
        occupied=tuple(occupied),
        document=document,
    )


def _catalog(suite_dir: Path) -> dict[str, domain.RegisteredSuite]:
    catalog: dict[str, domain.RegisteredSuite] = {}
    for path in sorted(suite_dir.glob("*.toml")):
        suite = suite_model.load(path)
        resolved = contracts.suite_to_dict(suite)
        catalog[path.stem] = domain.RegisteredSuite(
            id=path.stem,
            sha256=contracts.sha256_json(resolved),
            resolved=resolved,
            model=suite,
            path=path,
        )
    return catalog


def _available_runners(
    snapshot: domain.FleetSnapshot,
    start: datetime,
    end: datetime,
    count: int,
) -> tuple[domain.Runner, ...] | None:
    unavailable: set[str] = {
        runner.id for runner in snapshot.runners if runner.state == "failed"
    }
    for lease in snapshot.occupied:
        if start < lease.ends_at and end > lease.starts_at:
            unavailable.update(lease.runner_ids)

    for offset in range(0, len(snapshot.runners) - count + 1):
        candidate = snapshot.runners[offset : offset + count]
        numbers = [runner.number for runner in candidate]
        if numbers != list(range(numbers[0], numbers[0] + count)):
            continue
        if all(runner.id not in unavailable for runner in candidate):
            return candidate
    return None


def _runner_count(suite: suite_model.Suite, leased: int) -> int:
    caps = [
        task.max_runners or suite.meta.runners
        for task in suite.tasks
        if task.mode == "horizontal"
    ]
    if not caps:
        return 1
    return min(suite.meta.runners, leased, min(caps))


class Planner:
    """Owns command validation, suite resolution, admission, and placement."""

    def __init__(self, suite_dir: Path, max_manifests: int = MAX_MANIFESTS):
        self.suite_dir = suite_dir
        self.max_manifests = max_manifests

    def plan(
        self,
        command: dict[str, Any],
        snapshot_document: dict[str, Any],
        *,
        source: domain.Source,
        runner_image: str,
        now: datetime,
        ownership: str = "ad-hoc",
    ) -> domain.PlanningResult:
        try:
            snapshot = _snapshot(snapshot_document)
            payload = command["payload"]
            definition = _definition(payload)
            definition_toml = payload["definition_toml"]
            base_version = payload["base_snapshot_version"]
        except (KeyError, TypeError, ValueError) as error:
            return domain.Rejected(
                "invalid_definition",
                "The planning command is malformed.",
                ({"field": "command", "message": str(error)},),
            )

        if base_version != snapshot.version:
            return domain.Stale(
                "Fleet state changed. Validate this definition again.",
                snapshot.version,
            )

        diagnostics: list[dict[str, Any]] = []
        if command.get("operation") != "validate_batch":
            diagnostics.append(
                {"field": "operation", "message": "must be validate_batch"}
            )
        if command.get("repository", {}).get("id") != source.repository:
            diagnostics.append(
                {
                    "field": "repository.id",
                    "message": "does not match the immutable source repository",
                }
            )
        if not _REPOSITORY.fullmatch(source.repository):
            diagnostics.append(
                {
                    "field": "source.repository",
                    "message": "must be an owner/repository identity",
                }
            )
        if not _COMMIT.fullmatch(source.commit):
            diagnostics.append(
                {"field": "source.commit", "message": "must be a full 40-hex commit"}
            )
        if not _IMAGE.fullmatch(runner_image):
            diagnostics.append(
                {
                    "field": "runner_image",
                    "message": "must be an immutable ghcr.io sha256 reference",
                }
            )
        if ownership not in ("ad-hoc", "repository"):
            diagnostics.append(
                {"field": "ownership", "message": "must be ad-hoc or repository"}
            )
        if now.tzinfo is None:
            diagnostics.append(
                {"field": "evaluated_at", "message": "must include a UTC offset"}
            )

        try:
            parsed_toml = _toml_definition(definition_toml)
            if parsed_toml != _definition_toml_shape(definition):
                diagnostics.append(
                    {
                        "field": "definition_toml",
                        "message": "does not match the normalized definition",
                    }
                )
        except ValueError as error:
            diagnostics.append({"field": "definition_toml", "message": str(error)})

        end = definition.start + timedelta(milliseconds=definition.window_ms)
        if definition.start < snapshot.starts_at or end > snapshot.ends_at:
            diagnostics.append(
                {
                    "field": "start_utc",
                    "message": "requested window is outside the authoritative snapshot",
                }
            )
        if definition.runners < 1 or definition.runners > len(snapshot.runners):
            diagnostics.append(
                {
                    "field": "runners",
                    "message": f"must be between 1 and {len(snapshot.runners)}",
                }
            )

        try:
            catalog = _catalog(self.suite_dir)
        except suite_model.SuiteError as error:
            diagnostics.append({"field": "tests", "message": str(error)})
            catalog = {}
        selected: list[domain.RegisteredSuite] = []
        for index, suite_id in enumerate(definition.tests):
            suite = catalog.get(suite_id)
            if suite is None:
                diagnostics.append(
                    {
                        "field": f"tests[{index}]",
                        "message": f"unknown registered suite {suite_id!r}",
                    }
                )
            else:
                selected.append(suite)

        manifest_count = sum(
            _runner_count(suite.model, definition.runners) for suite in selected
        )
        if manifest_count > self.max_manifests:
            diagnostics.append(
                {
                    "field": "tests",
                    "message": f"materializes {manifest_count} manifests; limit is {self.max_manifests}",
                }
            )
        if diagnostics:
            return domain.Rejected(
                "invalid_definition",
                "The batch definition is not admissible.",
                tuple(diagnostics),
            )

        leased = _available_runners(snapshot, definition.start, end, definition.runners)
        if leased is None:
            latest_end = max(
                (
                    lease.ends_at
                    for lease in snapshot.occupied
                    if definition.start < lease.ends_at and end > lease.starts_at
                ),
                default=end,
            )
            return domain.Rejected(
                "no_capacity",
                f"No contiguous {definition.runners}-runner range is free for the requested window.",
                (
                    {
                        "field": "runners",
                        "message": "the requested lease overlaps authoritative ownership",
                    },
                ),
                (
                    {
                        "startsAt": _instant(latest_end),
                        "runners": max(1, definition.runners - 1),
                        "message": "retry after the active lease or request fewer runners",
                    },
                ),
            )

        try:
            accepted = self._accepted(
                command,
                snapshot,
                definition,
                definition_toml,
                selected,
                leased,
                source,
                runner_image,
                now.astimezone(UTC),
                ownership,
            )
        except ValueError as error:
            return domain.Rejected(
                "invalid_definition",
                "The batch budget cannot be placed.",
                ({"field": "window_hours", "message": str(error)},),
            )
        return domain.Accepted(accepted)

    def _accepted(
        self,
        command: dict[str, Any],
        snapshot: domain.FleetSnapshot,
        definition: domain.BatchDefinition,
        definition_toml: str,
        selected: list[domain.RegisteredSuite],
        leased: tuple[domain.Runner, ...],
        source: domain.Source,
        runner_image: str,
        now: datetime,
        ownership: str,
    ) -> domain.AcceptedPlan:
        identity_input = {
            "command": command,
            "snapshot_version": snapshot.version,
            "source": {"repository": source.repository, "commit": source.commit},
            "runner_image": runner_image,
            "evaluated_at": _instant(now),
            "ownership": ownership,
        }
        root_digest = contracts.sha256_json(identity_input)
        plan_id = f"plan_{root_digest[:20]}"
        batch_id = f"batch_{root_digest[20:40]}"
        plan_version = f"planv_{root_digest[40:56]}"
        batch_version = f"batchv_{root_digest[48:64]}"

        occurrences = Counter(definition.tests)
        seen: Counter[str] = Counter()
        raw_tasks: list[dict[str, Any]] = []
        runner_cursor = 0
        for suite_ordinal, suite in enumerate(selected):
            seen[suite.id] += 1
            instance = seen[suite.id] if occurrences[suite.id] > 1 else None
            total_runners = _runner_count(suite.model, len(leased))
            for runner_index in range(total_runners):
                runner = leased[(runner_cursor + runner_index) % len(leased)]
                task_digest = contracts.sha256_json(
                    {
                        "plan": plan_id,
                        "suite_ordinal": suite_ordinal,
                        "runner_index": runner_index,
                    }
                )
                raw_tasks.append(
                    {
                        "id": f"task_{task_digest[:20]}",
                        "slice_id": f"slice_{task_digest[20:40]}",
                        "manifest_id": f"manifest_{task_digest[40:60]}",
                        "campaign_id": f"campaign_{task_digest[:20]}",
                        "suite": suite,
                        "runner": runner,
                        "runner_index": runner_index,
                        "total_runners": total_runners,
                        "instance": instance,
                        "weight": sum(task.weight for task in suite.model.tasks),
                        "priority": max(task.priority for task in suite.model.tasks),
                    }
                )
            runner_cursor += total_runners

        by_runner: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for raw_task in raw_tasks:
            by_runner[raw_task["runner"].id].append(raw_task)

        planned: list[domain.PlannedTask] = []
        for runner_id, lane in by_runner.items():
            ordered = sorted(
                enumerate(lane), key=lambda item: (-item[1]["priority"], item[0])
            )
            weight_total = sum(item["weight"] for _, item in ordered)
            cursor = definition.start
            remaining_ms = definition.window_ms
            for position, (_, item) in enumerate(ordered):
                if position == len(ordered) - 1:
                    allocation_ms = remaining_ms
                else:
                    allocation_ms = int(
                        definition.window_ms * item["weight"] / weight_total
                    )
                    remaining_ms -= allocation_ms
                ratio = item["suite"].model.meta.overlap_ratio
                nominal_ms = int(allocation_ms * (1 - ratio))
                reserve_ms = max(2_000, min(300_000, allocation_ms // 10))
                soft_ms = min(
                    item["suite"].model.meta.budget_ms,
                    allocation_ms - reserve_ms,
                )
                required_ms = max(
                    task.boot.host_timeout_ms for task in item["suite"].model.tasks
                )
                if soft_ms < required_ms:
                    raise ValueError(
                        f"{item['suite'].id} cannot fit a {required_ms}ms host boot "
                        f"in its {allocation_ms}ms runner slice"
                    )
                hard_ms = soft_ms + reserve_ms // 2
                actions_ms = max(hard_ms + 1, soft_ms + reserve_ms)
                nominal_end = cursor + timedelta(milliseconds=nominal_ms)
                task_end = cursor + timedelta(milliseconds=allocation_ms)
                seed: int | None = int(
                    contracts.sha256_json({"plan": plan_id, "task": item["id"]})[:16],
                    16,
                )
                if not any(
                    task.nightmare.wants_seed for task in item["suite"].model.tasks
                ):
                    seed = None
                planned.append(
                    domain.PlannedTask(
                        id=item["id"],
                        slice_id=item["slice_id"],
                        manifest_id=item["manifest_id"],
                        campaign_id=item["campaign_id"],
                        suite_id=item["suite"].id,
                        runner_id=runner_id,
                        runner_index=item["runner_index"],
                        total_runners=item["total_runners"],
                        instance=item["instance"],
                        starts_at=cursor,
                        nominal_ends_at=nominal_end,
                        ends_at=task_end,
                        soft_budget_ms=soft_ms,
                        hard_budget_ms=hard_ms,
                        actions_job_budget_ms=actions_ms,
                        base_seed=seed,
                    )
                )
                cursor = task_end

        planned.sort(
            key=lambda planned_task: (
                planned_task.starts_at,
                planned_task.runner_id,
                planned_task.id,
            )
        )
        groups_by_digest: dict[str, list[str]] = defaultdict(list)
        configurations: dict[str, dict[str, Any]] = {}
        suite_by_id = {suite.id: suite for suite in selected}
        for planned_task in planned:
            configuration = suite_by_id[planned_task.suite_id].resolved["build"]
            request_digest = contracts.sha256_json(
                {
                    "source": {
                        "repository": source.repository,
                        "commit": source.commit,
                    },
                    "runner_image": runner_image,
                    "configuration": configuration,
                }
            )
            groups_by_digest[request_digest].append(planned_task.id)
            configurations[request_digest] = configuration
        build_groups = tuple(
            domain.BuildGroup(
                id=f"build_{digest[:20]}",
                request_sha256=digest,
                configuration=configurations[digest],
                task_ids=tuple(sorted(task_ids)),
            )
            for digest, task_ids in sorted(groups_by_digest.items())
        )

        suites = tuple({suite.id: suite for suite in selected}.values())
        document = self._wire(
            plan_id,
            plan_version,
            batch_id,
            batch_version,
            snapshot.version,
            now,
            source,
            runner_image,
            definition,
            definition_toml,
            ownership,
            leased,
            tuple(planned),
            suites,
            build_groups,
        )
        return domain.AcceptedPlan(
            id=plan_id,
            version=plan_version,
            base_snapshot_version=snapshot.version,
            evaluated_at=now,
            expires_at=now + PLAN_TTL,
            source=source,
            runner_image=runner_image,
            definition=definition,
            definition_toml=definition_toml,
            definition_sha256=contracts.sha256_json(_definition_wire(definition)),
            ownership=ownership,
            batch_id=batch_id,
            batch_version=batch_version,
            runner_ids=tuple(runner.id for runner in leased),
            tasks=tuple(planned),
            suites=suites,
            build_groups=build_groups,
            document=document,
        )

    def _wire(
        self,
        plan_id: str,
        plan_version: str,
        batch_id: str,
        batch_version: str,
        base_snapshot_version: str,
        now: datetime,
        source: domain.Source,
        runner_image: str,
        definition: domain.BatchDefinition,
        definition_toml: str,
        ownership: str,
        leased: tuple[domain.Runner, ...],
        tasks: tuple[domain.PlannedTask, ...],
        suites: tuple[domain.RegisteredSuite, ...],
        build_groups: tuple[domain.BuildGroup, ...],
    ) -> dict[str, Any]:
        suite_by_id = {suite.id: suite for suite in suites}
        group_for_task = {
            task_id: group.id for group in build_groups for task_id in group.task_ids
        }
        batch_tasks = []
        manifest_requests = []
        for task in tasks:
            suite = suite_by_id[task.suite_id]
            ratio = suite.model.meta.overlap_ratio
            batch_tasks.append(
                {
                    "id": task.id,
                    "name": task.suite_id,
                    "family": task.suite_id.split("_", 1)[0],
                    "instance": task.instance,
                    "runnerId": task.runner_id,
                    "state": "idle",
                    "health": "healthy",
                    "color": definition.color,
                    "started": False,
                    "cancelled": False,
                    "slice": {
                        "id": task.slice_id,
                        "taskId": task.id,
                        "runnerId": task.runner_id,
                        "startsAt": _instant(task.starts_at),
                        "endsAt": _instant(task.ends_at),
                        "nominalEndsAt": _instant(task.nominal_ends_at),
                        "immutable": False,
                        "trace": [],
                    },
                    "elasticBuffer": {
                        "startsAt": _instant(task.nominal_ends_at),
                        "endsAt": _instant(task.ends_at),
                        "ratio": ratio,
                        "immutable": True,
                    },
                }
            )
            manifest_requests.append(
                {
                    "manifestId": task.manifest_id,
                    "taskId": task.id,
                    "suiteId": task.suite_id,
                    "buildGroupId": group_for_task[task.id],
                    "runnerId": task.runner_id,
                    "runnerIndex": task.runner_index,
                    "totalRunners": task.total_runners,
                    "campaignId": task.campaign_id,
                    "baseSeed": hex(task.base_seed)
                    if task.base_seed is not None
                    else None,
                    "softBudgetMs": task.soft_budget_ms,
                    "hardBudgetMs": task.hard_budget_ms,
                    "actionsJobBudgetMs": task.actions_job_budget_ms,
                }
            )

        start = definition.start
        end = start + timedelta(milliseconds=definition.window_ms)
        definition_wire = _definition_wire(definition)
        return {
            "schemaVersion": 1,
            "id": plan_id,
            "version": plan_version,
            "baseSnapshotVersion": base_snapshot_version,
            "evaluatedAt": _instant(now),
            "expiresAt": _instant(now + PLAN_TTL),
            "source": {"repository": source.repository, "commit": source.commit},
            "runnerImage": runner_image,
            "definition": {
                "ownership": ownership,
                "toml": definition_toml,
                "sha256": contracts.sha256_json(definition_wire),
                "normalized": definition_wire,
            },
            "suites": [
                {"id": suite.id, "sha256": suite.sha256, "resolved": suite.resolved}
                for suite in suites
            ],
            "buildGroups": [
                {
                    "id": group.id,
                    "requestSha256": group.request_sha256,
                    "configuration": group.configuration,
                    "taskIds": list(group.task_ids),
                }
                for group in build_groups
            ],
            "manifests": manifest_requests,
            "batch": {
                "id": batch_id,
                "version": batch_version,
                "name": definition.name,
                "color": definition.color,
                "lifecycle": "draft",
                "health": "healthy",
                "ownership": ownership,
                "template": "Custom"
                if ownership == "ad-hoc"
                else "Repository schedule",
                "definitionToml": definition_toml,
                "lease": {
                    "runnerIds": [runner.id for runner in leased],
                    "startsAt": _instant(start),
                    "endsAt": _instant(end),
                    "policyReserveMinutes": 0,
                },
                "tasks": batch_tasks,
                "startedPrefixTaskIds": [],
                "residualTail": (
                    {
                        "startsAt": _instant(start),
                        "endsAt": _instant(end),
                        "mutableTaskIds": [task.id for task in tasks],
                    }
                    if ownership == "ad-hoc"
                    else None
                ),
                "updatedAt": _instant(now),
            },
        }


def render_result(result: domain.PlanningResult) -> dict[str, Any]:
    if isinstance(result, domain.Accepted):
        return {"kind": "accepted", "plan": result.plan.document}
    if isinstance(result, domain.Stale):
        return {
            "kind": "stale",
            "code": result.code,
            "message": result.message,
            "currentVersion": result.current_version,
        }
    return {
        "kind": "rejected",
        "code": result.code,
        "message": result.message,
        "diagnostics": list(result.diagnostics),
        "alternatives": list(result.alternatives),
    }


def write_plan(result: domain.PlanningResult, path: Path) -> None:
    path.write_text(
        json.dumps(render_result(result), indent=2) + "\n", encoding="utf-8"
    )
