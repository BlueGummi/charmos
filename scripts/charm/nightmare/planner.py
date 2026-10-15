"""Deterministic admission and placement of one committed suite."""

import re
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from . import contracts
from . import suite as suite_model

START_DELAY = timedelta(minutes=5)


@dataclass(frozen=True)
class Source:
    repository: str
    commit: str


@dataclass(frozen=True)
class PlannedTask:
    id: str
    manifest_id: str
    campaign_id: str
    suite_id: str
    runner_id: str
    runner_index: int
    total_runners: int
    soft_budget_ms: int
    hard_budget_ms: int
    actions_job_budget_ms: int
    base_seed: int | None


@dataclass(frozen=True)
class BuildGroup:
    id: str
    request_sha256: str
    configuration: dict[str, Any]
    task_ids: tuple[str, ...]


@dataclass(frozen=True)
class AcceptedPlan:
    id: str
    batch_id: str
    tasks: tuple[PlannedTask, ...]
    build_groups: tuple[BuildGroup, ...]
    document: dict[str, Any]


@dataclass(frozen=True)
class Accepted:
    plan: AcceptedPlan
    kind: str = "accepted"


@dataclass(frozen=True)
class Rejected:
    code: str
    message: str
    diagnostics: tuple[dict[str, Any], ...]
    kind: str = "rejected"


PlanningResult = Accepted | Rejected


@dataclass(frozen=True)
class BundleReceipt:
    bundle_id: str
    sha256: str
    request_sha256: str


@dataclass(frozen=True)
class PlanBundle:
    plan_id: str
    build_groups: tuple[BuildGroup, ...]
    manifests: tuple[dict[str, Any], ...]

    @property
    def matrix(self) -> tuple[dict[str, str], ...]:
        return tuple(
            {
                "manifest_id": manifest["manifest_id"],
                "manifest_artifact": manifest["result"]["artifact_name"],
                "bundle_id": manifest["build"]["bundle_id"],
            }
            for manifest in self.manifests
        )


MAX_MANIFESTS = 64
_COMMIT = re.compile(r"^[0-9a-f]{40}$")
_IMAGE = re.compile(r"^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$")
_REPOSITORY = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")


def _runner_count(suite: suite_model.Suite) -> int:
    caps = [
        task.max_runners or suite.meta.runners
        for task in suite.tasks
        if task.mode == "horizontal"
    ]
    return min(suite.meta.runners, min(caps)) if caps else 1


def _reject(*diagnostics: dict[str, Any]) -> Rejected:
    return Rejected(
        "invalid_definition", "The suite is not admissible.", tuple(diagnostics)
    )


def plan(
    suite_dir: Path,
    suite_id: str,
    *,
    source: Source,
    runner_image: str,
    now: datetime,
    max_manifests: int = MAX_MANIFESTS,
) -> PlanningResult:
    """Place one committed suite on `suite.meta.runners` runners, one task each."""
    diagnostics: list[dict[str, Any]] = []
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
    if now.tzinfo is None:
        diagnostics.append(
            {"field": "evaluated_at", "message": "must include a UTC offset"}
        )
    path = suite_dir / f"{suite_id}.toml"
    suite: suite_model.Suite | None = None
    try:
        suite = suite_model.load(path)
        if suite.meta.name != suite_id:
            diagnostics.append(
                {
                    "field": "suite",
                    "message": f"{suite_id!r} does not match {suite.meta.name!r}",
                }
            )
    except suite_model.SuiteError as error:
        diagnostics.append({"field": "suite", "message": str(error)})
    if diagnostics or suite is None:
        return _reject(*diagnostics)

    runners = _runner_count(suite)
    if runners > max_manifests:
        return _reject(
            {
                "field": "suite",
                "message": f"materializes {runners} manifests; limit is {max_manifests}",
            }
        )
    resolved = suite.resolved()
    suite_sha = contracts.sha256_json(resolved)
    now = now.astimezone(UTC)
    root_digest = contracts.sha256_json(
        {
            "suite": suite_sha,
            "source": {"repository": source.repository, "commit": source.commit},
            "runner_image": runner_image,
            "evaluated_at": contracts.instant(now),
        }
    )
    plan_id = f"plan_{root_digest[:20]}"
    batch_id = f"batch_{root_digest[20:40]}"

    # Each runner owns the whole suite budget; the host-boot floor is the only
    # thing that can make the slice unplaceable.
    window_ms = suite.meta.budget_ms
    reserve_ms = max(2_000, min(300_000, window_ms // 10))
    soft_ms = min(suite.meta.budget_ms, window_ms - reserve_ms)
    required_ms = max(task.boot.host_timeout_ms for task in suite.tasks)
    if soft_ms < required_ms:
        return _reject(
            {
                "field": "window_hours",
                "message": f"{suite_id} cannot fit a {required_ms}ms host boot "
                f"in its {window_ms}ms runner slice",
            }
        )
    hard_ms = soft_ms + reserve_ms // 2
    actions_ms = max(hard_ms + 1, soft_ms + reserve_ms)
    wants_seed = any(task.nightmare.wants_seed for task in suite.tasks)

    tasks: list[PlannedTask] = []
    for runner_index in range(runners):
        digest = contracts.sha256_json(
            {"plan": plan_id, "suite_ordinal": 0, "runner_index": runner_index}
        )
        task_id = f"task_{digest[:20]}"
        seed = int(contracts.sha256_json({"plan": plan_id, "task": task_id})[:16], 16)
        tasks.append(
            PlannedTask(
                id=task_id,
                manifest_id=f"manifest_{digest[40:60]}",
                campaign_id=f"campaign_{digest[:20]}",
                suite_id=suite_id,
                runner_id=f"runner_{runner_index + 1}",
                runner_index=runner_index,
                total_runners=runners,
                soft_budget_ms=soft_ms,
                hard_budget_ms=hard_ms,
                actions_job_budget_ms=actions_ms,
                base_seed=seed if wants_seed else None,
            )
        )
    tasks.sort(key=lambda task: (task.runner_id, task.id))

    configuration = resolved["build"]
    request_digest = contracts.sha256_json(
        {
            "source": {"repository": source.repository, "commit": source.commit},
            "runner_image": runner_image,
            "configuration": configuration,
        }
    )
    group = BuildGroup(
        id=f"build_{request_digest[:20]}",
        request_sha256=request_digest,
        configuration=configuration,
        task_ids=tuple(sorted(task.id for task in tasks)),
    )
    document = {
        "schemaVersion": 1,
        "id": plan_id,
        "batchId": batch_id,
        "evaluatedAt": contracts.instant(now),
        "source": {"repository": source.repository, "commit": source.commit},
        "runnerImage": runner_image,
        "suites": [{"id": suite_id, "sha256": suite_sha, "resolved": resolved}],
        "buildGroups": [
            {
                "id": group.id,
                "requestSha256": group.request_sha256,
                "configuration": group.configuration,
                "taskIds": list(group.task_ids),
            }
        ],
        "manifests": [
            {
                "manifestId": task.manifest_id,
                "taskId": task.id,
                "suiteId": task.suite_id,
                "buildGroupId": group.id,
                "runnerId": task.runner_id,
                "runnerIndex": task.runner_index,
                "totalRunners": task.total_runners,
                "campaignId": task.campaign_id,
                "baseSeed": hex(task.base_seed) if task.base_seed is not None else None,
                "softBudgetMs": task.soft_budget_ms,
                "hardBudgetMs": task.hard_budget_ms,
                "actionsJobBudgetMs": task.actions_job_budget_ms,
            }
            for task in tasks
        ],
    }
    return Accepted(AcceptedPlan(plan_id, batch_id, tuple(tasks), (group,), document))


def render_result(result: PlanningResult) -> dict[str, Any]:
    if isinstance(result, Accepted):
        return {"kind": "accepted", "plan": result.plan.document}
    return {
        "kind": "rejected",
        "code": result.code,
        "message": result.message,
        "diagnostics": list(result.diagnostics),
    }
