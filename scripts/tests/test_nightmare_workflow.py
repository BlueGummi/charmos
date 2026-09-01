from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

from charm.nightmare import workflow

NOW = datetime(2026, 9, 1, 12, 0, tzinfo=UTC)


def test_inline_command_preserves_one_atomic_batch() -> None:
    command, snapshot = workflow.inline_command(
        toml_text="""[batch]
name = "Lock checker"
start_utc = "2026-09-01T12:05:00Z"
window_hours = 1
runners = 2
color = "#a7c080"
tests = ["overnight_locks"]
""",
        repository="axvonx/charmos",
        ref="main",
        now=NOW,
        runner_capacity=12,
    )

    assert command["operation"] == "validate_batch"
    assert command["payload"]["definition"]["tests"] == ["overnight_locks"]
    assert command["payload"]["definition"]["runners"] == 2
    assert snapshot["batches"] == []


def test_inline_command_rejects_malformed_batch() -> None:
    with pytest.raises(ValueError, match="missing \\[batch\\] table"):
        workflow.inline_command(
            toml_text="[suite]\nname = 'not-a-batch'",
            repository="axvonx/charmos",
            ref="main",
            now=NOW,
        )


def test_workflow_dispatch_is_batch_scoped_and_validates_before_queueing() -> None:
    text = Path(".github/workflows/nightmare-orchestrator.yml").read_text()
    assert "batch_id:" in text
    assert "claim-{1}" in text
    assert text.index("- name: Validate and place") < text.index(
        "- name: Classify immediate or deferred execution"
    )
    assert "ownership=ad-hoc" in text
    assert "wait-until" not in text
    assert "queued_run_id:" in text
    assert "name: nightmare-queue" in text
    assert text.count("needs.plan.outputs.deferred != 'true'") == 2


def test_future_inline_command_becomes_durable_queue_metadata() -> None:
    command, _ = workflow.inline_command(
        toml_text=f"""[batch]
name = "Later"
start_utc = "{(NOW + timedelta(minutes=20)).isoformat()}"
window_hours = 1
runners = 1
color = "#a7c080"
tests = ["harness_smoke"]
""",
        repository="axvonx/charmos",
        ref="main",
        now=NOW,
    )

    metadata = workflow.queue_metadata(
        command,
        batch_id="batch-later",
        source_sha="abc123",
        now=NOW,
    )

    assert metadata["deferred"] is True
    assert metadata["batch_id"] == "batch-later"
    assert metadata["source_sha"] == "abc123"


def test_due_inline_command_executes_without_queueing() -> None:
    command, _ = workflow.inline_command(
        toml_text=f"""[batch]
name = "Now"
start_utc = "{NOW.isoformat()}"
window_hours = 1
runners = 1
color = "#a7c080"
tests = ["harness_smoke"]
""",
        repository="axvonx/charmos",
        ref="main",
        now=NOW,
    )

    metadata = workflow.queue_metadata(
        command,
        batch_id="batch-now",
        source_sha="abc123",
        now=NOW,
    )

    assert metadata["deferred"] is False


def test_deferred_command_cannot_outlive_its_queue_artifact() -> None:
    command, _ = workflow.inline_command(
        toml_text=f"""[batch]
name = "Too far"
start_utc = "{(NOW + timedelta(days=30)).isoformat()}"
window_hours = 1
runners = 1
color = "#a7c080"
tests = ["harness_smoke"]
""",
        repository="axvonx/charmos",
        ref="main",
        now=NOW,
    )

    with pytest.raises(ValueError, match="29-day queue horizon"):
        workflow.queue_metadata(
            command,
            batch_id="batch-too-far",
            source_sha="abc123",
            now=NOW,
        )


def test_periodic_waker_is_lightweight_and_can_dispatch() -> None:
    text = Path(".github/workflows/nightmare-waker.yml").read_text()
    assert "cron: '7/15 * * * *'" in text
    assert "actions: write" in text
    assert "runs-on: ubuntu-slim" in text
    assert "nightmare wake-queue" in text
