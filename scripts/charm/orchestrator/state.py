"""Durable Nightmare orchestration vocabulary and wire representation."""

from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime
from enum import StrEnum
from typing import Any


def instant(value: datetime) -> str:
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def parse_instant(value: str) -> datetime:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("durable timestamps must include a UTC offset")
    return parsed.astimezone(UTC)


class CoordinatorStatus(StrEnum):
    SLEEPING = "sleeping"
    WAKING = "waking"
    RUNNING = "running"


class CommandStatus(StrEnum):
    QUEUED = "queued"
    APPLIED = "applied"
    REJECTED = "rejected"


class AttemptStatus(StrEnum):
    QUEUED = "queued"
    RUNNING = "running"
    COMPLETED = "completed"
    RECOVERY_REQUIRED = "recovery_required"


@dataclass(frozen=True)
class CoordinatorLease:
    owner: str
    wake_id: str
    generation: int
    expires_at: str


@dataclass(frozen=True)
class CoordinatorState:
    status: str = CoordinatorStatus.SLEEPING.value
    generation: int = 0
    lease: CoordinatorLease | None = None
    idle_since: str | None = None


@dataclass(frozen=True)
class CommandEntry:
    command_id: str
    idempotency_key: str
    sha256: str
    generation: int
    status: str
    received_at: str
    document: dict[str, Any]
    result: dict[str, Any] | None = None
    request_id: str | None = None


@dataclass(frozen=True)
class AttemptEntry:
    attempt_id: str
    plan_id: str
    generation: int
    manifest_ids: tuple[str, ...]
    result_digests: dict[str, str] = field(default_factory=dict)
    missing_artifacts: tuple[str, ...] = ()
    status: str = AttemptStatus.QUEUED.value


@dataclass(frozen=True)
class OutboxEntry:
    id: str
    generation: int
    reason: str
    command_ref: str
    created_at: str
    dispatched_at: str | None = None
    external_id: str | None = None


@dataclass(frozen=True)
class OrchestratorState:
    schema_version: int = 1
    coordinator: CoordinatorState = field(default_factory=CoordinatorState)
    commands: dict[str, CommandEntry] = field(default_factory=dict)
    idempotency: dict[str, dict[str, str]] = field(default_factory=dict)
    plans: dict[str, dict[str, Any]] = field(default_factory=dict)
    snapshots: dict[str, dict[str, Any]] = field(default_factory=dict)
    batches: dict[str, dict[str, Any]] = field(default_factory=dict)
    batch_plans: dict[str, str] = field(default_factory=dict)
    attempts: dict[str, AttemptEntry] = field(default_factory=dict)
    outbox: dict[str, OutboxEntry] = field(default_factory=dict)
    authority_revision: int = 0

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, document: dict[str, Any]) -> "OrchestratorState":
        coordinator_doc = document.get("coordinator", {})
        lease_doc = coordinator_doc.get("lease")
        coordinator = CoordinatorState(
            status=coordinator_doc.get("status", CoordinatorStatus.SLEEPING.value),
            generation=int(coordinator_doc.get("generation", 0)),
            lease=CoordinatorLease(**lease_doc) if lease_doc else None,
            idle_since=coordinator_doc.get("idle_since"),
        )
        commands = {
            key: CommandEntry(**value)
            for key, value in document.get("commands", {}).items()
        }
        attempts = {
            key: AttemptEntry(
                **{
                    **value,
                    "manifest_ids": tuple(value["manifest_ids"]),
                    "missing_artifacts": tuple(value.get("missing_artifacts", [])),
                }
            )
            for key, value in document.get("attempts", {}).items()
        }
        outbox = {
            key: OutboxEntry(**value)
            for key, value in document.get("outbox", {}).items()
        }
        return cls(
            schema_version=int(document.get("schema_version", 1)),
            coordinator=coordinator,
            commands=commands,
            idempotency=dict(document.get("idempotency", {})),
            plans=dict(document.get("plans", {})),
            snapshots=dict(document.get("snapshots", {})),
            batches=dict(document.get("batches", {})),
            batch_plans=dict(document.get("batch_plans", {})),
            attempts=attempts,
            outbox=outbox,
            authority_revision=int(document.get("authority_revision", 0)),
        )
