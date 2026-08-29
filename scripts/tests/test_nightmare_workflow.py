from datetime import UTC, datetime
from pathlib import Path

import pytest

from charm.nightmare import workflow as OW


def test_repository_command_builds_valid_inputs() -> None:
    now = datetime(2026, 8, 29, 3, 17, tzinfo=UTC)
    command, snapshot = OW.repository_command(
        suite_id="p7_m3_short",
        suite_dir=Path("nightmare/suites"),
        repository="axvonx/charmos",
        ref="main",
        now=now,
        runner_capacity=12,
    )
    assert command["operation"] == "validate_batch"
    assert command["payload"]["definition"]["name"] == "Repository p7_m3_short"
    assert command["payload"]["definition"]["runners"] == 1
    assert len(snapshot["runners"]) == 12
    assert snapshot["repository"]["name"] == "charmos"


def test_inline_command_valid_toml() -> None:
    now = datetime(2026, 8, 29, 12, 0, tzinfo=UTC)
    toml_text = """[batch]
name = "Ad-Hoc Stress Run"
start_utc = "2026-08-29T12:05:00Z"
window_hours = 2
runners = 4
color = "#e67e80"
tests = ["p7_m3_short", "overnight_locks"]
"""
    command, snapshot = OW.inline_command(
        toml_text=toml_text,
        repository="axvonx/charmos",
        ref="feature/experiment",
        now=now,
        runner_capacity=8,
    )
    assert command["operation"] == "validate_batch"
    assert command["payload"]["definition"]["name"] == "Ad-Hoc Stress Run"
    assert command["payload"]["definition"]["runners"] == 4
    assert command["payload"]["definition"]["tests"] == [
        "p7_m3_short",
        "overnight_locks",
    ]
    assert command["payload"]["definition_toml"] == toml_text
    assert len(snapshot["runners"]) == 8
    assert snapshot["repository"]["ref"] == "feature/experiment"


def test_inline_command_invalid_toml_syntax() -> None:
    now = datetime(2026, 8, 29, 12, 0, tzinfo=UTC)
    with pytest.raises(ValueError, match="invalid batch TOML"):
        OW.inline_command(
            toml_text="[batch\nname = invalid",
            repository="axvonx/charmos",
            ref="main",
            now=now,
        )


def test_inline_command_missing_required_fields() -> None:
    now = datetime(2026, 8, 29, 12, 0, tzinfo=UTC)
    with pytest.raises(ValueError, match="missing \\[batch\\] table"):
        OW.inline_command(
            toml_text="[suite]\nname = 'wrong'",
            repository="axvonx/charmos",
            ref="main",
            now=now,
        )

    with pytest.raises(ValueError, match="runners must be a positive integer"):
        OW.inline_command(
            toml_text="[batch]\nname = 'Test'\nwindow_hours = 1\nrunners = 0\ntests = ['p7_m3_short']",
            repository="axvonx/charmos",
            ref="main",
            now=now,
        )
