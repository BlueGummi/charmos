"""Versioned data contracts at the Nightmare execution seam."""

import hashlib
import json
import re
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import Any, Literal, NewType, NotRequired, TypeAlias, TypedDict

from . import grammar
from . import suite as suite_model

SCHEMA_VERSION = 1

ManifestId = NewType("ManifestId", str)
PlanId = NewType("PlanId", str)
BatchId = NewType("BatchId", str)
TaskId = NewType("TaskId", str)
BuildId = NewType("BuildId", str)
CommandId = NewType("CommandId", str)
RunnerId = NewType("RunnerId", str)
SnapshotVersion = NewType("SnapshotVersion", str)


class FieldDiagnostic(TypedDict):
    field: str
    message: str
    line: NotRequired[int]
    column: NotRequired[int]


class ValidationAccepted(TypedDict):
    kind: Literal["accepted"]
    plan: dict[str, Any]


class ValidationRejected(TypedDict):
    kind: Literal["rejected"]
    code: Literal["invalid_definition", "no_capacity"]
    message: str
    diagnostics: list[FieldDiagnostic]
    alternatives: list[dict[str, Any]]


class ValidationStale(TypedDict):
    kind: Literal["stale"]
    code: Literal["snapshot_changed"]
    message: str
    currentVersion: SnapshotVersion


ValidationResult: TypeAlias = ValidationAccepted | ValidationRejected | ValidationStale


class SubmissionAccepted(TypedDict):
    kind: Literal["accepted"]
    snapshot: dict[str, Any]
    batch: dict[str, Any]


class SubmissionRejected(TypedDict):
    kind: Literal["rejected"]
    code: Literal["submission_rejected"]
    message: str


class SubmissionStale(TypedDict):
    kind: Literal["stale"]
    code: Literal["plan_expired", "snapshot_changed"]
    message: str
    currentVersion: SnapshotVersion


SubmissionResult: TypeAlias = SubmissionAccepted | SubmissionRejected | SubmissionStale


class TailAccepted(TypedDict):
    kind: Literal["accepted"]
    plan: dict[str, Any]


class TailRejected(TypedDict):
    kind: Literal["rejected"]
    code: Literal["immutable", "no_tail_capacity", "commit_rejected"]
    message: str
    diagnostics: NotRequired[list[FieldDiagnostic]]


class TailConflict(TypedDict):
    kind: Literal["conflict"]
    code: Literal["batch_changed"]
    message: str
    currentVersion: SnapshotVersion


TailResult: TypeAlias = TailAccepted | TailRejected | TailConflict

_ID_RE = re.compile(r"^[a-z][a-z0-9_:-]{0,127}$")
_REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
_IMAGE_RE = re.compile(
    r"^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$"
)
_ARTIFACT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


class DiscoveryKind(StrEnum):
    NONE = "none"
    FINDING = "finding"
    CRASH = "crash"
    STALL = "stall"
    MIXED = "mixed"


class ExecutionHealth(StrEnum):
    HEALTHY = "healthy"
    INFRASTRUCTURE = "infrastructure"
    PARTIAL = "partial"


class ExecutionLifecycle(StrEnum):
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"


@dataclass(frozen=True)
class Diagnostic:
    path: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}: {self.message}"


class ContractError(ValueError):
    def __init__(self, source: str, diagnostics: list[Diagnostic]):
        self.source = source
        self.diagnostics = diagnostics
        body = "\n".join(f"  {diagnostic}" for diagnostic in diagnostics)
        super().__init__(f"{source}: {len(diagnostics)} problem(s)\n{body}")


@dataclass(frozen=True)
class SourceIdentity:
    repository: str
    commit: str


@dataclass(frozen=True)
class SuiteIdentity:
    id: str
    sha256: str
    resolved: dict[str, Any]
    model: suite_model.Suite


@dataclass(frozen=True)
class BuildIdentity:
    bundle_id: BuildId
    sha256: str
    runner_image: str


@dataclass(frozen=True)
class CampaignContract:
    campaign_id: str
    runner_index: int
    total_runners: int
    base_seed: int | None
    soft_budget_ms: int
    hard_budget_ms: int
    actions_job_budget_ms: int
    gate_first: bool
    dry_run: bool


@dataclass(frozen=True)
class ResultTarget:
    schema_version: int
    artifact_name: str


@dataclass(frozen=True)
class RunnerManifest:
    schema_version: int
    manifest_id: ManifestId
    plan_id: PlanId
    batch_id: BatchId
    task_id: TaskId
    attempt: int
    source: SourceIdentity
    suite: SuiteIdentity
    build: BuildIdentity
    campaign: CampaignContract
    result: ResultTarget
    document: dict[str, Any]


def canonical_json(value: Any) -> bytes:
    """The byte representation used by contract digests."""
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def sha256_json(value: Any) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def suite_to_dict(suite: suite_model.Suite) -> dict[str, Any]:
    """Resolve defaults so a manifest is independent of loader evolution."""
    return {
        "suite": {
            "name": suite.meta.name,
            "runners": suite.meta.runners,
            "budget_hours": suite.meta.budget_hours,
            "overlap_ratio": suite.meta.overlap_ratio,
        },
        "build": {
            "compiler": suite.build.compiler,
            "type": suite.build.type,
            "cmake_definitions": list(suite.build.cmake_definitions),
            "smp": {
                "sockets": suite.build.smp.sockets,
                "cores": suite.build.smp.cores,
                "threads": suite.build.smp.threads,
            },
            "memory_mib": suite.build.memory_mib,
        },
        "tasks": [
            {
                "name": task.name,
                "mode": task.mode,
                "weight": task.weight,
                "priority": task.priority,
                **(
                    {"max_runners": task.max_runners}
                    if task.max_runners is not None
                    else {}
                ),
                "boot": {
                    "duration_ms": task.boot.duration_ms,
                    "drain_grace_ms": task.boot.drain_grace_ms,
                    "timeout_ms": task.boot.timeout_ms,
                    "gate_first": task.boot.gate_first,
                    "max_boots": task.boot.max_boots,
                    "min_interval_ms": task.boot.min_interval_ms,
                    "stat_interval_ms": task.boot.stat_interval_ms,
                    "stall_threshold_ms": task.boot.stall_threshold_ms,
                    "on_stall": task.boot.on_stall,
                },
                "nightmare": {
                    "intensity": task.nightmare.intensity,
                    "seed_mode": task.nightmare.seed_mode,
                    "perturb": list(task.nightmare.perturb),
                    "perturb_opts": task.nightmare.perturb_opts,
                    "opts": task.nightmare.opts,
                },
            }
            for task in suite.tasks
        ],
    }


def _object(
    value: Any,
    path: str,
    required: frozenset[str],
    diagnostics: list[Diagnostic],
) -> dict[str, Any]:
    if not isinstance(value, dict):
        diagnostics.append(Diagnostic(path, "expected an object"))
        return {}
    keys = set(value)
    for key in sorted(required - keys):
        diagnostics.append(Diagnostic(f"{path}.{key}", "is required"))
    for key in sorted(keys - required):
        diagnostics.append(Diagnostic(f"{path}.{key}", "is not allowed"))
    return value


def _string(
    value: Any,
    path: str,
    diagnostics: list[Diagnostic],
    pattern: re.Pattern[str] | None = None,
) -> str:
    if not isinstance(value, str):
        diagnostics.append(Diagnostic(path, "expected a string"))
        return ""
    if pattern is not None and not pattern.fullmatch(value):
        diagnostics.append(Diagnostic(path, "has an invalid format"))
    return value


def _integer(
    value: Any,
    path: str,
    diagnostics: list[Diagnostic],
    minimum: int = 0,
) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        diagnostics.append(Diagnostic(path, "expected an integer"))
        return minimum
    if value < minimum:
        diagnostics.append(Diagnostic(path, f"must be at least {minimum}"))
    return value


def _boolean(value: Any, path: str, diagnostics: list[Diagnostic]) -> bool:
    if not isinstance(value, bool):
        diagnostics.append(Diagnostic(path, "expected a boolean"))
        return False
    return value


def validate_manifest(document: Any) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    root = _object(
        document,
        "manifest",
        frozenset(
            {
                "schema_version",
                "manifest_id",
                "plan_id",
                "batch_id",
                "task_id",
                "attempt",
                "source",
                "suite",
                "build",
                "campaign",
                "result",
            }
        ),
        diagnostics,
    )

    if root.get("schema_version") != SCHEMA_VERSION:
        diagnostics.append(
            Diagnostic("manifest.schema_version", f"must be {SCHEMA_VERSION}")
        )
    for key in ("manifest_id", "plan_id", "batch_id", "task_id"):
        _string(root.get(key), f"manifest.{key}", diagnostics, _ID_RE)
    _integer(root.get("attempt"), "manifest.attempt", diagnostics, minimum=1)

    source = _object(
        root.get("source"),
        "manifest.source",
        frozenset({"repository", "commit"}),
        diagnostics,
    )
    _string(
        source.get("repository"),
        "manifest.source.repository",
        diagnostics,
        _REPOSITORY_RE,
    )
    _string(
        source.get("commit"),
        "manifest.source.commit",
        diagnostics,
        _COMMIT_RE,
    )

    suite = _object(
        root.get("suite"),
        "manifest.suite",
        frozenset({"id", "sha256", "resolved"}),
        diagnostics,
    )
    suite_id = _string(suite.get("id"), "manifest.suite.id", diagnostics, _ID_RE)
    suite_digest = _string(
        suite.get("sha256"), "manifest.suite.sha256", diagnostics, _SHA256_RE
    )
    resolved = suite.get("resolved")
    if not isinstance(resolved, dict):
        diagnostics.append(Diagnostic("manifest.suite.resolved", "expected an object"))
    else:
        if suite_digest and sha256_json(resolved) != suite_digest:
            diagnostics.append(
                Diagnostic(
                    "manifest.suite.sha256",
                    "does not match the canonical resolved suite",
                )
            )
        try:
            model = suite_model.from_dict(resolved, source="manifest.suite.resolved")
        except suite_model.SuiteError as error:
            diagnostics.extend(
                Diagnostic(f"manifest.suite.resolved.{item.path}", item.message)
                for item in error.diagnostics
            )
        else:
            if suite_id and model.meta.name != suite_id:
                diagnostics.append(
                    Diagnostic(
                        "manifest.suite.id",
                        f"does not match resolved suite name {model.meta.name!r}",
                    )
                )

    build = _object(
        root.get("build"),
        "manifest.build",
        frozenset({"bundle_id", "sha256", "runner_image"}),
        diagnostics,
    )
    _string(
        build.get("bundle_id"),
        "manifest.build.bundle_id",
        diagnostics,
        _ID_RE,
    )
    _string(
        build.get("sha256"),
        "manifest.build.sha256",
        diagnostics,
        _SHA256_RE,
    )
    _string(
        build.get("runner_image"),
        "manifest.build.runner_image",
        diagnostics,
        _IMAGE_RE,
    )

    campaign = _object(
        root.get("campaign"),
        "manifest.campaign",
        frozenset(
            {
                "campaign_id",
                "runner_index",
                "total_runners",
                "base_seed",
                "soft_budget_ms",
                "hard_budget_ms",
                "actions_job_budget_ms",
                "gate_first",
                "dry_run",
            }
        ),
        diagnostics,
    )
    _string(
        campaign.get("campaign_id"),
        "manifest.campaign.campaign_id",
        diagnostics,
        _ID_RE,
    )
    runner_index = _integer(
        campaign.get("runner_index"),
        "manifest.campaign.runner_index",
        diagnostics,
    )
    total_runners = _integer(
        campaign.get("total_runners"),
        "manifest.campaign.total_runners",
        diagnostics,
        minimum=1,
    )
    if total_runners > 0 and runner_index >= total_runners:
        diagnostics.append(
            Diagnostic(
                "manifest.campaign.runner_index",
                "must be less than total_runners",
            )
        )
    base_seed = campaign.get("base_seed")
    if base_seed is not None:
        if not isinstance(base_seed, str):
            diagnostics.append(
                Diagnostic("manifest.campaign.base_seed", "expected a string or null")
            )
        else:
            try:
                grammar.parse_uint(base_seed)
            except grammar.GrammarError as error:
                diagnostics.append(
                    Diagnostic("manifest.campaign.base_seed", str(error))
                )

    soft = _integer(
        campaign.get("soft_budget_ms"),
        "manifest.campaign.soft_budget_ms",
        diagnostics,
        minimum=1,
    )
    hard = _integer(
        campaign.get("hard_budget_ms"),
        "manifest.campaign.hard_budget_ms",
        diagnostics,
        minimum=1,
    )
    actions = _integer(
        campaign.get("actions_job_budget_ms"),
        "manifest.campaign.actions_job_budget_ms",
        diagnostics,
        minimum=1,
    )
    if not soft < hard:
        diagnostics.append(
            Diagnostic(
                "manifest.campaign.hard_budget_ms",
                "must be greater than soft_budget_ms",
            )
        )
    if not hard < actions:
        diagnostics.append(
            Diagnostic(
                "manifest.campaign.actions_job_budget_ms",
                "must be greater than hard_budget_ms",
            )
        )
    _boolean(
        campaign.get("gate_first"),
        "manifest.campaign.gate_first",
        diagnostics,
    )
    _boolean(campaign.get("dry_run"), "manifest.campaign.dry_run", diagnostics)

    if isinstance(resolved, dict):
        try:
            model = suite_model.from_dict(resolved, source="manifest.suite.resolved")
        except suite_model.SuiteError:
            pass
        else:
            required = max(task.boot.host_timeout_ms for task in model.tasks)
            if required > soft:
                diagnostics.append(
                    Diagnostic(
                        "manifest.campaign.soft_budget_ms",
                        f"must fit one host boot timeout ({required}ms)",
                    )
                )

    result = _object(
        root.get("result"),
        "manifest.result",
        frozenset({"schema_version", "artifact_name"}),
        diagnostics,
    )
    if result.get("schema_version") != SCHEMA_VERSION:
        diagnostics.append(
            Diagnostic("manifest.result.schema_version", f"must be {SCHEMA_VERSION}")
        )
    _string(
        result.get("artifact_name"),
        "manifest.result.artifact_name",
        diagnostics,
        _ARTIFACT_RE,
    )
    return diagnostics


def manifest_from_dict(document: Any, source: str = "<memory>") -> RunnerManifest:
    diagnostics = validate_manifest(document)
    if diagnostics:
        raise ContractError(source, diagnostics)
    assert isinstance(document, dict)
    source_doc = document["source"]
    suite_doc = document["suite"]
    build_doc = document["build"]
    campaign_doc = document["campaign"]
    result_doc = document["result"]
    resolved = suite_doc["resolved"]
    model = suite_model.from_dict(resolved, source=f"{source}:suite")
    base_seed = campaign_doc["base_seed"]
    return RunnerManifest(
        schema_version=SCHEMA_VERSION,
        manifest_id=ManifestId(document["manifest_id"]),
        plan_id=PlanId(document["plan_id"]),
        batch_id=BatchId(document["batch_id"]),
        task_id=TaskId(document["task_id"]),
        attempt=document["attempt"],
        source=SourceIdentity(source_doc["repository"], source_doc["commit"]),
        suite=SuiteIdentity(suite_doc["id"], suite_doc["sha256"], resolved, model),
        build=BuildIdentity(
            BuildId(build_doc["bundle_id"]),
            build_doc["sha256"],
            build_doc["runner_image"],
        ),
        campaign=CampaignContract(
            campaign_id=campaign_doc["campaign_id"],
            runner_index=campaign_doc["runner_index"],
            total_runners=campaign_doc["total_runners"],
            base_seed=(grammar.parse_uint(base_seed) if base_seed else None),
            soft_budget_ms=campaign_doc["soft_budget_ms"],
            hard_budget_ms=campaign_doc["hard_budget_ms"],
            actions_job_budget_ms=campaign_doc["actions_job_budget_ms"],
            gate_first=campaign_doc["gate_first"],
            dry_run=campaign_doc["dry_run"],
        ),
        result=ResultTarget(result_doc["schema_version"], result_doc["artifact_name"]),
        document=document,
    )


def load_manifest(path: Path) -> RunnerManifest:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ContractError(
            str(path), [Diagnostic("manifest", f"invalid JSON: {error}")]
        ) from None
    except OSError as error:
        raise ContractError(
            str(path), [Diagnostic("manifest", f"cannot read: {error}")]
        ) from None
    return manifest_from_dict(document, source=str(path))
