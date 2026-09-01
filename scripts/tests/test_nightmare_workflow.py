from datetime import UTC, datetime
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


def test_workflow_dispatch_is_batch_scoped_and_validates_before_waiting() -> None:
    text = Path(".github/workflows/nightmare-orchestrator.yml").read_text()
    assert "batch_id:" in text
    assert "run-name: Nightmare" in text
    assert text.index("- name: Validate and place") < text.index(
        "- name: Wait for accepted ad-hoc start time"
    )
    assert "ownership=ad-hoc" in text
