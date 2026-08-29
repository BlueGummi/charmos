"""CAS store interface plus local and production adapters."""

import json
import threading
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any, Protocol

from . import state as S


@dataclass(frozen=True)
class StateEnvelope:
    version: int
    state: S.OrchestratorState
    observed_at: datetime


class OrchestratorStore(Protocol):
    def read(self) -> StateEnvelope: ...

    def compare_and_swap(
        self, expected_version: int, state: S.OrchestratorState
    ) -> StateEnvelope | None: ...


class MemoryStore:
    """Thread-safe store adapter with an injected authoritative clock."""

    def __init__(
        self,
        state: S.OrchestratorState | None = None,
        *,
        clock: Callable[[], datetime] = lambda: datetime.now(UTC),
    ) -> None:
        self._state = state or S.OrchestratorState()
        self._version = 0
        self._clock = clock
        self._lock = threading.Lock()

    def read(self) -> StateEnvelope:
        with self._lock:
            return StateEnvelope(self._version, self._state, self._clock())

    def compare_and_swap(
        self, expected_version: int, state: S.OrchestratorState
    ) -> StateEnvelope | None:
        with self._lock:
            if self._version != expected_version:
                return None
            self._version += 1
            self._state = state
            return StateEnvelope(self._version, self._state, self._clock())


POSTGRES_MIGRATION = """
CREATE TABLE IF NOT EXISTS nightmare_orchestrator_state (
    singleton SMALLINT PRIMARY KEY CHECK (singleton = 1),
    version BIGINT NOT NULL,
    schema_version INTEGER NOT NULL,
    document JSONB NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
INSERT INTO nightmare_orchestrator_state
    (singleton, version, schema_version, document)
VALUES (1, 0, 1, '{}'::jsonb)
ON CONFLICT (singleton) DO NOTHING;
"""


class PostgresStore:
    """Serializable PostgreSQL adapter; one state document is one authority commit."""

    def __init__(self, connect: Callable[[], Any]) -> None:
        self._connect = connect

    @classmethod
    def from_dsn(cls, dsn: str) -> "PostgresStore":
        try:
            import psycopg
        except ImportError as error:  # pragma: no cover - production dependency
            raise RuntimeError(
                "install charmos-tools[orchestrator] for PostgreSQL"
            ) from error
        return cls(lambda: psycopg.connect(dsn))

    def migrate(self) -> None:
        with self._connect() as connection, connection.cursor() as cursor:
            cursor.execute(POSTGRES_MIGRATION)

    def read(self) -> StateEnvelope:
        with self._connect() as connection, connection.cursor() as cursor:
            cursor.execute(
                "SELECT version, document, CURRENT_TIMESTAMP "
                "FROM nightmare_orchestrator_state WHERE singleton = 1"
            )
            row = cursor.fetchone()
        if row is None:
            raise RuntimeError("PostgreSQL orchestrator state is not initialized")
        version, document, observed_at = row
        if isinstance(document, str):
            document = json.loads(document)
        state = S.OrchestratorState.from_dict(document or {})
        return StateEnvelope(int(version), state, observed_at.astimezone(UTC))

    def compare_and_swap(
        self, expected_version: int, state: S.OrchestratorState
    ) -> StateEnvelope | None:
        payload = json.dumps(state.to_dict(), separators=(",", ":"), sort_keys=True)
        with self._connect() as connection, connection.cursor() as cursor:
            cursor.execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE")
            cursor.execute(
                "UPDATE nightmare_orchestrator_state "
                "SET version = version + 1, schema_version = %s, "
                "document = %s::jsonb, updated_at = CURRENT_TIMESTAMP "
                "WHERE singleton = 1 AND version = %s "
                "RETURNING version, CURRENT_TIMESTAMP",
                (state.schema_version, payload, expected_version),
            )
            row = cursor.fetchone()
            if row is None:
                connection.rollback()
                return None
        version, observed_at = row
        return StateEnvelope(int(version), state, observed_at.astimezone(UTC))
