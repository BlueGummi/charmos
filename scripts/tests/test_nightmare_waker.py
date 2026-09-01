from datetime import UTC, datetime, timedelta

from charm.nightmare import waker

NOW = datetime(2026, 9, 1, 12, 0, tzinfo=UTC)


def test_select_due_queues_skips_future_and_claimed_runs() -> None:
    queues = [
        waker.QueuedPlan(11, "batch-due", NOW - timedelta(minutes=1)),
        waker.QueuedPlan(12, "batch-future", NOW + timedelta(minutes=1)),
        waker.QueuedPlan(13, "batch-claimed", NOW - timedelta(minutes=2)),
    ]

    assert waker.select_due(queues, claimed_run_ids={13}, now=NOW) == [queues[0]]


def test_select_due_orders_oldest_start_first() -> None:
    queues = [
        waker.QueuedPlan(11, "batch-second", NOW - timedelta(minutes=1)),
        waker.QueuedPlan(12, "batch-first", NOW - timedelta(minutes=10)),
    ]

    assert [queue.run_id for queue in waker.select_due(queues, set(), NOW)] == [12, 11]


def test_claimed_queue_ids_preserve_the_operator_batch_name() -> None:
    runs = [
        {"display_title": "Nightmare · batch-smoke · claim-123"},
        {"display_title": "Nightmare · batch-smoke"},
    ]

    assert waker.claimed_queue_run_ids(runs) == {123}
