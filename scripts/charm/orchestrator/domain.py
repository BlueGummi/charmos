"""Typed values crossing the authoritative planning seam."""

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from ..nightmare import suite as suite_model


@dataclass(frozen=True)
class BatchDefinition:
    name: str
    start: datetime
    window_ms: int
    runners: int
    color: str
    tests: tuple[str, ...]


@dataclass(frozen=True)
class Source:
    repository: str
    commit: str


@dataclass(frozen=True)
class Runner:
    id: str
    number: int
    state: str
    label: str


@dataclass(frozen=True)
class OccupiedLease:
    runner_ids: frozenset[str]
    starts_at: datetime
    ends_at: datetime


@dataclass(frozen=True)
class FleetSnapshot:
    version: str
    starts_at: datetime
    ends_at: datetime
    runners: tuple[Runner, ...]
    occupied: tuple[OccupiedLease, ...]
    document: dict[str, Any]


@dataclass(frozen=True)
class RegisteredSuite:
    id: str
    sha256: str
    resolved: dict[str, Any]
    model: suite_model.Suite
    path: Path


@dataclass(frozen=True)
class PlannedTask:
    id: str
    slice_id: str
    manifest_id: str
    campaign_id: str
    suite_id: str
    runner_id: str
    runner_index: int
    total_runners: int
    instance: int | None
    starts_at: datetime
    nominal_ends_at: datetime
    ends_at: datetime
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
    version: str
    base_snapshot_version: str
    evaluated_at: datetime
    expires_at: datetime
    source: Source
    runner_image: str
    definition: BatchDefinition
    definition_toml: str
    definition_sha256: str
    ownership: str
    batch_id: str
    batch_version: str
    runner_ids: tuple[str, ...]
    tasks: tuple[PlannedTask, ...]
    suites: tuple[RegisteredSuite, ...]
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
    alternatives: tuple[dict[str, Any], ...] = ()
    kind: str = "rejected"


@dataclass(frozen=True)
class Stale:
    message: str
    current_version: str
    code: str = "snapshot_changed"
    kind: str = "stale"


PlanningResult = Accepted | Rejected | Stale


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
