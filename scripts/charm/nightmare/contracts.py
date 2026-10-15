"""Versioned data contracts at the Nightmare execution seam."""

import hashlib
import json
from datetime import UTC, datetime
from enum import StrEnum
from functools import cached_property
from pathlib import Path
from typing import Annotated, Any, Literal

from pydantic import (
    AfterValidator,
    BaseModel,
    ConfigDict,
    Field,
    ValidationError,
)

from . import grammar
from . import suite as suite_model
from .suite import Diagnostic, diagnostics_of

SCHEMA_VERSION = 1

Id = Annotated[str, Field(pattern=r"^[a-z][a-z0-9_:-]{0,127}$")]
Repository = Annotated[str, Field(pattern=r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")]
Sha256 = Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")]
Commit = Annotated[str, Field(pattern=r"^[0-9a-f]{40}$")]
Image = Annotated[
    str,
    Field(pattern=r"^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$"),
]
ArtifactName = Annotated[str, Field(pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")]
Seed = Annotated[str, AfterValidator(lambda v: grammar.parse_uint(v) and v)]


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


class ContractError(ValueError):
    def __init__(self, source: str, diagnostics: list[Diagnostic]):
        self.source = source
        self.diagnostics = diagnostics
        body = "\n".join(f"  {diagnostic}" for diagnostic in diagnostics)
        super().__init__(f"{source}: {len(diagnostics)} problem(s)\n{body}")


class Contract(BaseModel):
    model_config = ConfigDict(strict=True, frozen=True, extra="forbid")

    @property
    def document(self) -> dict[str, Any]:
        return self.model_dump(mode="json")


class SourceIdentity(Contract):
    repository: Repository
    commit: Commit


class SuiteIdentity(Contract):
    id: Id
    sha256: Sha256
    resolved: dict[str, Any]

    @cached_property
    def model(self) -> suite_model.Suite:
        return suite_model.from_dict(self.resolved, source="manifest.suite.resolved")


class BuildIdentity(Contract):
    bundle_id: Id
    sha256: Sha256
    runner_image: Image


class CampaignContract(Contract):
    campaign_id: Id
    runner_index: int = Field(ge=0)
    total_runners: int = Field(ge=1)
    base_seed: Seed | None
    soft_budget_ms: int = Field(ge=1)
    hard_budget_ms: int = Field(ge=1)
    actions_job_budget_ms: int = Field(ge=1)
    gate_first: bool
    dry_run: bool

    @property
    def seed(self) -> int | None:
        return grammar.parse_uint(self.base_seed) if self.base_seed else None


class ResultTarget(Contract):
    schema_version: Literal[1]
    artifact_name: ArtifactName


class RunnerManifest(Contract):
    schema_version: Literal[1]
    manifest_id: Id
    plan_id: Id
    batch_id: Id
    task_id: Id
    attempt: int = Field(ge=1)
    source: SourceIdentity
    suite: SuiteIdentity
    build: BuildIdentity
    campaign: CampaignContract
    result: ResultTarget


def instant(value: datetime) -> str:
    """A UTC ISO-8601 instant with a `Z` suffix; naive values are taken as UTC."""
    if value.tzinfo is None:
        value = value.replace(tzinfo=UTC)
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


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


def _manifest_diagnostics(m: RunnerManifest) -> list[Diagnostic]:
    """What the field types cannot say on their own"""
    d: list[Diagnostic] = []

    def bad(path: str, message: str) -> None:
        d.append(Diagnostic(path=f"manifest.{path}", message=message))

    c = m.campaign
    if sha256_json(m.suite.resolved) != m.suite.sha256:
        bad("suite.sha256", "does not match the canonical resolved suite")
    if c.runner_index >= c.total_runners:
        bad("campaign.runner_index", "must be less than total_runners")
    if not c.soft_budget_ms < c.hard_budget_ms:
        bad("campaign.hard_budget_ms", "must be greater than soft_budget_ms")
    if not c.hard_budget_ms < c.actions_job_budget_ms:
        bad("campaign.actions_job_budget_ms", "must be greater than hard_budget_ms")

    try:
        model = m.suite.model
    except suite_model.SuiteError as error:
        for item in error.diagnostics:
            bad(f"suite.resolved.{item.path}", item.message)
        return d
    if model.meta.name != m.suite.id:
        bad("suite.id", f"does not match resolved suite name {model.meta.name!r}")
    required = max(task.boot.host_timeout_ms for task in model.tasks)
    if required > c.soft_budget_ms:
        bad("campaign.soft_budget_ms", f"must fit one host boot timeout ({required}ms)")
    return d


def _parse_manifest(document: Any) -> tuple[RunnerManifest | None, list[Diagnostic]]:
    try:
        manifest = RunnerManifest.model_validate(document)
    except ValidationError as error:
        return None, diagnostics_of(error, "manifest")
    return manifest, _manifest_diagnostics(manifest)


def validate_manifest(document: Any) -> list[Diagnostic]:
    return _parse_manifest(document)[1]


def manifest_from_dict(document: Any, source: str = "<memory>") -> RunnerManifest:
    manifest, diagnostics = _parse_manifest(document)
    if diagnostics or manifest is None:
        raise ContractError(source, diagnostics)
    return manifest


def load_manifest(path: Path) -> RunnerManifest:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ContractError(
            str(path), [Diagnostic(path="manifest", message=f"invalid JSON: {error}")]
        ) from None
    except OSError as error:
        raise ContractError(
            str(path), [Diagnostic(path="manifest", message=f"cannot read: {error}")]
        ) from None
    return manifest_from_dict(document, source=str(path))
