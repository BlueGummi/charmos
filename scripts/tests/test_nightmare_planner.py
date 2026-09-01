from datetime import UTC, datetime
from pathlib import Path

from charm.nightmare import planner, workflow


def test_real_lock_suite_plans_as_one_batch_with_private_runner_fanout() -> None:
    now = datetime(2026, 9, 1, 12, 0, tzinfo=UTC)
    command, snapshot = workflow.inline_command(
        toml_text="""[batch]
name = "Real lock soak"
start_utc = "2026-09-01T12:05:00Z"
window_hours = 6
runners = 3
color = "#a7c080"
tests = ["overnight_locks"]
""",
        repository="axvonx/charmos",
        ref="main",
        now=now,
        runner_capacity=12,
    )
    result = planner.Planner(Path("nightmare/suites")).plan(
        command,
        snapshot,
        source=planner.Source("axvonx/charmos", "a" * 40),
        runner_image="ghcr.io/axvonx/charmos-x86-env@sha256:" + "b" * 64,
        now=now,
        ownership="ad-hoc",
    )

    assert isinstance(result, planner.Accepted)
    assert result.plan.document["batch"]["id"] == result.plan.batch_id
    assert result.plan.document["batch"]["residualTail"] is not None
    assert len(result.plan.tasks) == 3
    assert {task.suite_id for task in result.plan.tasks} == {"overnight_locks"}
