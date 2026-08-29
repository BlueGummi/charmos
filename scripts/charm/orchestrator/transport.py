"""Execution transport interface and GitHub Actions adapter."""

import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Protocol

from . import state as S


@dataclass(frozen=True)
class DispatchReceipt:
    external_id: str


class ExecutionTransport(Protocol):
    def dispatch(self, wake: S.OutboxEntry) -> DispatchReceipt: ...


class MemoryTransport:
    def __init__(self) -> None:
        self.dispatched: dict[str, S.OutboxEntry] = {}

    def dispatch(self, wake: S.OutboxEntry) -> DispatchReceipt:
        self.dispatched.setdefault(wake.id, wake)
        return DispatchReceipt(f"memory:{wake.id}")


class GitHubActionsTransport:
    """Dispatch one narrow doorbell; the workflow consumes durable authority."""

    def __init__(
        self,
        *,
        repository: str,
        workflow: str,
        ref: str,
        token: str,
        opener: Any = urllib.request.urlopen,
    ) -> None:
        self.repository = repository
        self.workflow = workflow
        self.ref = ref
        self._token = token
        self._opener = opener

    def dispatch(self, wake: S.OutboxEntry) -> DispatchReceipt:
        url = (
            f"https://api.github.com/repos/{self.repository}/actions/workflows/"
            f"{self.workflow}/dispatches"
        )
        body = json.dumps(
            {
                "ref": self.ref,
                "inputs": {
                    "protocol_version": "1",
                    "wake_id": wake.id,
                    "reason": wake.reason,
                    "command_ref": wake.command_ref,
                },
            }
        ).encode()
        request = urllib.request.Request(
            url,
            data=body,
            method="POST",
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self._token}",
                "Content-Type": "application/json",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        try:
            with self._opener(request, timeout=30) as response:
                status = response.status
        except (urllib.error.URLError, TimeoutError) as error:
            raise RuntimeError(f"GitHub Actions dispatch failed: {error}") from error
        if status != 204:
            raise RuntimeError(f"GitHub Actions dispatch returned HTTP {status}")
        return DispatchReceipt(f"github:{self.repository}:{wake.id}")
