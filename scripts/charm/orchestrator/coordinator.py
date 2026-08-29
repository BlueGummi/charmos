"""CAS coordinator that commits authority before delivering outbox effects."""

from dataclasses import dataclass

from . import reconciler as R
from .store import OrchestratorStore, StateEnvelope
from .transport import ExecutionTransport


class ConcurrentUpdate(RuntimeError):
    pass


@dataclass(frozen=True)
class CoordinatedResult:
    envelope: StateEnvelope
    result: dict[str, object]


class Coordinator:
    def __init__(
        self,
        store: OrchestratorStore,
        transport: ExecutionTransport,
        *,
        retries: int = 8,
    ) -> None:
        self.store = store
        self.transport = transport
        self.reconciler = R.Reconciler()
        self.retries = retries

    def apply(self, event: R.Event) -> CoordinatedResult:
        for _attempt in range(self.retries):
            envelope = self.store.read()
            transition = self.reconciler.evolve(
                envelope.state, event, now=envelope.observed_at
            )
            if not transition.changed:
                return CoordinatedResult(envelope, transition.result)
            committed = self.store.compare_and_swap(envelope.version, transition.state)
            if committed is not None:
                self.deliver()
                return CoordinatedResult(committed, transition.result)
        raise ConcurrentUpdate("orchestrator state changed during every CAS retry")

    def deliver(self) -> None:
        """Deliver committed wakes; duplicate calls are idempotent by wake ID."""
        while True:
            envelope = self.store.read()
            pending = next(
                (
                    entry
                    for entry in sorted(
                        envelope.state.outbox.values(), key=lambda item: item.id
                    )
                    if entry.external_id is None
                ),
                None,
            )
            if pending is None:
                return
            receipt = self.transport.dispatch(pending)
            transition = self.reconciler.evolve(
                envelope.state,
                R.MarkDispatched(pending.id, receipt.external_id),
                now=envelope.observed_at,
            )
            if self.store.compare_and_swap(envelope.version, transition.state) is None:
                continue
