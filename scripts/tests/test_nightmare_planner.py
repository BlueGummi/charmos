from datetime import UTC, datetime

from charm.nightmare import planner
from charm.paths import nightmare_dir

NOW = datetime(2026, 9, 1, 12, 0, tzinfo=UTC)
SOURCE = planner.Source("axvonx/charmos", "a" * 40)
IMAGE = "ghcr.io/axvonx/charmos-x86-env@sha256:" + "b" * 64


def plan(suite: str, **kw):
    return planner.plan(
        nightmare_dir() / "suites",
        suite,
        source=kw.pop("source", SOURCE),
        runner_image=IMAGE,
        now=NOW,
        **kw,
    )


def test_real_lock_suite_fans_out_one_task_per_runner() -> None:
    result = plan("overnight_locks")

    assert isinstance(result, planner.Accepted)
    assert result.plan.document["batchId"] == result.plan.batch_id
    assert [task.runner_id for task in result.plan.tasks] == [
        "runner_1",
        "runner_2",
        "runner_3",
    ]
    assert {task.suite_id for task in result.plan.tasks} == {"overnight_locks"}
    assert len(result.plan.build_groups) == 1


def test_planning_is_deterministic() -> None:
    assert plan("overnight_wake") == plan("overnight_wake")


def test_a_bad_source_commit_is_rejected_with_its_field() -> None:
    result = plan("harness_smoke", source=planner.Source("axvonx/charmos", "nope"))

    assert isinstance(result, planner.Rejected)
    assert [d["field"] for d in result.diagnostics] == ["source.commit"]


def test_an_unknown_suite_is_rejected() -> None:
    assert isinstance(plan("no_such_suite"), planner.Rejected)


def test_more_manifests_than_the_limit_is_rejected() -> None:
    assert isinstance(plan("overnight_locks", max_manifests=2), planner.Rejected)
