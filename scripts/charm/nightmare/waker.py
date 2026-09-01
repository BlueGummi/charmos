"""Artifact-backed wakeup adapter for deferred Nightmare plans."""

from __future__ import annotations

import io
import json
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any


@dataclass(frozen=True)
class QueuedPlan:
    run_id: int
    batch_id: str
    start: datetime


def select_due(
    queues: list[QueuedPlan],
    claimed_run_ids: set[int],
    now: datetime | None = None,
) -> list[QueuedPlan]:
    evaluated_at = (now or datetime.now(UTC)).astimezone(UTC)
    return sorted(
        (
            queue
            for queue in queues
            if queue.run_id not in claimed_run_ids
            and queue.start.astimezone(UTC) <= evaluated_at
        ),
        key=lambda queue: (queue.start, queue.run_id),
    )


def claimed_queue_run_ids(runs: list[dict[str, Any]]) -> set[int]:
    claim_marker = " · claim-"
    claimed: set[int] = set()
    for run in runs:
        title = run.get("display_title")
        if not isinstance(title, str) or claim_marker not in title:
            continue
        run_id = title.rsplit(claim_marker, 1)[1]
        if run_id.isdigit():
            claimed.add(int(run_id))
    return claimed


class GitHubQueueClient:
    def __init__(self, token: str, api_url: str = "https://api.github.com") -> None:
        if not token:
            raise ValueError("GitHub token is required")
        self.token = token
        self.api_url = api_url.rstrip("/")

    def _request(
        self,
        path_or_url: str,
        *,
        method: str = "GET",
        body: dict[str, Any] | None = None,
    ) -> bytes:
        url = (
            path_or_url
            if path_or_url.startswith(("https://", "http://"))
            else f"{self.api_url}{path_or_url}"
        )
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(
            url,
            data=data,
            method=method,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self.token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "charmos-nightmare-waker",
            },
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()

    def _json(self, path: str) -> dict[str, Any]:
        value = json.loads(self._request(path))
        if not isinstance(value, dict):
            raise ValueError(f"GitHub returned a non-object for {path}")
        return value

    def _pages(self, path: str, key: str) -> list[dict[str, Any]]:
        separator = "&" if "?" in path else "?"
        items: list[dict[str, Any]] = []
        page = 1
        while True:
            body = self._json(f"{path}{separator}per_page=100&page={page}")
            values = body.get(key)
            if not isinstance(values, list):
                raise ValueError(f"GitHub response is missing {key}")
            page_items = [value for value in values if isinstance(value, dict)]
            items.extend(page_items)
            if len(values) < 100:
                return items
            page += 1

    def discover(
        self, repository: str, workflow: str
    ) -> tuple[list[QueuedPlan], set[int]]:
        repo = urllib.parse.quote(repository, safe="/")
        workflow_name = urllib.parse.quote(workflow, safe="")
        workflow_data = self._json(f"/repos/{repo}/actions/workflows/{workflow_name}")
        workflow_id = workflow_data.get("id")
        if not isinstance(workflow_id, int):
            raise ValueError("GitHub workflow response is missing its numeric ID")

        runs = self._pages(
            f"/repos/{repo}/actions/workflows/{workflow_name}/runs?event=workflow_dispatch",
            "workflow_runs",
        )
        valid_run_ids = {
            run_id
            for run in runs
            if run.get("workflow_id") == workflow_id
            and isinstance((run_id := run.get("id")), int)
        }
        claimed = claimed_queue_run_ids(runs)

        artifacts = self._pages(
            f"/repos/{repo}/actions/artifacts?name=nightmare-queue", "artifacts"
        )
        queues: list[QueuedPlan] = []
        for artifact in artifacts:
            if (
                artifact.get("expired") is True
                or artifact.get("name") != "nightmare-queue"
            ):
                continue
            run = artifact.get("workflow_run")
            run_id = run.get("id") if isinstance(run, dict) else None
            archive_url = artifact.get("archive_download_url")
            if (
                not isinstance(run_id, int)
                or run_id not in valid_run_ids
                or run_id in claimed
                or not isinstance(archive_url, str)
            ):
                continue
            with zipfile.ZipFile(io.BytesIO(self._request(archive_url))) as archive:
                metadata = json.loads(archive.read("queue.json"))
            batch_id = metadata.get("batch_id") if isinstance(metadata, dict) else None
            start_raw = (
                metadata.get("start_utc") if isinstance(metadata, dict) else None
            )
            if not isinstance(batch_id, str) or not isinstance(start_raw, str):
                raise ValueError(f"queue artifact from run {run_id} is malformed")
            start = datetime.fromisoformat(start_raw.replace("Z", "+00:00"))
            if start.tzinfo is None:
                raise ValueError(f"queue artifact from run {run_id} has a naive start")
            queues.append(QueuedPlan(run_id, batch_id, start))

        return queues, claimed

    def dispatch_claim(
        self, repository: str, workflow: str, ref: str, queue: QueuedPlan
    ) -> None:
        repo = urllib.parse.quote(repository, safe="/")
        workflow_name = urllib.parse.quote(workflow, safe="")
        self._request(
            f"/repos/{repo}/actions/workflows/{workflow_name}/dispatches",
            method="POST",
            body={
                "ref": ref,
                "inputs": {
                    "queued_run_id": str(queue.run_id),
                    "batch_id": queue.batch_id,
                },
            },
        )


def wake_due(
    *,
    token: str,
    repository: str,
    workflow: str,
    ref: str,
    api_url: str = "https://api.github.com",
    now: datetime | None = None,
) -> list[QueuedPlan]:
    client = GitHubQueueClient(token, api_url)
    queues, claimed = client.discover(repository, workflow)
    due = select_due(queues, claimed, now)
    for queue in due:
        client.dispatch_claim(repository, workflow, ref, queue)
    return due
