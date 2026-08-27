import json
from pathlib import Path

import pytest

from charm.cli import build_parser
from charm.nightmare import campaign as NC
from charm.nightmare import suite as NS


def create_sample_suite(
    name: str = "test_suite",
    budget_hours: float = 1.0,
    runners: int = 2,
    tasks: list[dict] | None = None,
) -> NS.Suite:
    if tasks is None:
        tasks = [
            {
                "name": "task_a",
                "mode": "horizontal",
                "weight": 1.0,
                "boot": {
                    "duration_ms": 1000,
                    "drain_grace_ms": 2000,
                    "stat_interval_ms": 100,
                    "max_boots": 3,
                },
                "nightmare": {
                    "seed_mode": "seedless",
                },
            }
        ]

    doc = {
        "suite": {
            "name": name,
            "budget_hours": budget_hours,
            "runners": runners,
        },
        "tasks": tasks,
    }
    return NS.from_dict(doc, source="inline")


class MockBootRunner:
    def __init__(self, boot_results: list[NC.BootResult] | None = None):
        self.boot_results = list(boot_results or [])
        self.call_count = 0
        self.invocations: list[dict] = []

    def run_boot(
        self,
        manifest: NC.CampaignManifest,
        task: NS.Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> NC.BootResult:
        self.invocations.append(
            {
                "task": task.name,
                "boot_index": boot_index,
                "cmdline": cmdline,
                "timeout_ms": timeout_ms,
            }
        )
        self.call_count += 1
        if self.boot_results:
            return self.boot_results.pop(0)
        return NC.BootResult(
            boot_index=boot_index,
            task_name=task.name,
            cmdline=cmdline,
            seed=None,
            duration_ms=100,
            exit_code=0,
            status=NC.BootStatus.OK.value,
            reason="completed",
            progress=500,
            findings=[],
        )


def test_campaign_clock_budget_and_remaining():
    current_time = 100.0

    def mock_time():
        return current_time

    clock = NC.CampaignClock(budget_ms=10000, time_fn=mock_time)
    assert clock.remaining_ms() == 10000
    assert clock.elapsed_ms() == 0

    current_time = 104.0
    assert clock.elapsed_ms() == 4000
    assert clock.remaining_ms() == 6000

    # fits into margins
    assert clock.can_fit_boot(boot_duration_ms=1000, flush_margin_ms=2000) is True
    assert clock.can_fit_boot(boot_duration_ms=5000, flush_margin_ms=2000) is False


def test_campaign_clock_interval_enforcement():
    current_time = 100.0
    slept = 0.0

    def mock_time():
        return current_time

    def mock_sleep(s):
        nonlocal current_time, slept
        slept += s
        current_time += s

    clock = NC.CampaignClock(budget_ms=10000, time_fn=mock_time, sleep_fn=mock_sleep)
    # started at 100.0, current 100.2, min_interval 500ms, should sleep 300ms
    current_time = 100.2
    clock.enforce_min_interval(min_interval_ms=500, last_boot_started_at_s=100.0)
    assert pytest.approx(slept) == 0.3


def test_boot_scheduler_horizontal():
    tasks = [
        NS.Task(
            name="task1",
            mode="horizontal",
            weight=1.0,
            boot=NS.Boot(duration_ms=1000, max_boots=2),
            nightmare=NS.Nightmare(seed_mode="seedless"),
        ),
        NS.Task(
            name="task2",
            mode="horizontal",
            weight=1.0,
            boot=NS.Boot(duration_ms=1000, max_boots=2),
            nightmare=NS.Nightmare(seed_mode="seedless"),
        ),
    ]

    scheduler = NC.BootScheduler(tasks, runner_index=0, total_runners=1)
    schedule = list(scheduler)
    assert len(schedule) == 4
    names = [t.name for t, _, _ in schedule]
    assert names.count("task1") == 2
    assert names.count("task2") == 2


def test_boot_scheduler_seed_partitioning():
    tasks = [
        NS.Task(
            name="seeded_task",
            mode="vertical",
            weight=1.0,
            boot=NS.Boot(duration_ms=1000, max_boots=3),
            nightmare=NS.Nightmare(seed_mode="split"),
        ),
    ]

    s0 = NC.BootScheduler(tasks, runner_index=0, total_runners=2, base_seed=0x1000)
    seeds0 = [seed for _, _, seed in s0]
    assert seeds0 == [0x1000, 0x1001, 0x1002]

    s1 = NC.BootScheduler(tasks, runner_index=1, total_runners=2, base_seed=0x1000)
    seeds1 = [seed for _, _, seed in s1]
    assert seeds1 == [0x1003, 0x1004, 0x1005]

    # Distinct non-overlapping seed partitions
    assert set(seeds0).isdisjoint(set(seeds1))


def test_signature_ledger_dedup_and_tiered_caps():
    ledger = NC.SignatureLedger(confident_cap=2)

    # Confident findings
    f_conf = [
        NC.FindingRecord(
            sig="sig_conf_1",
            tier="confident",
            kind="assert",
            site="kernel/lock.c:42",
            msg="mutex corrupted",
            boot_index=0,
            raw={"id": 1},
        ),
        NC.FindingRecord(
            sig="sig_conf_1",
            tier="confident",
            kind="assert",
            site="kernel/lock.c:42",
            msg="mutex corrupted",
            boot_index=1,
            raw={"id": 2},
        ),
        NC.FindingRecord(
            sig="sig_conf_1",
            tier="confident",
            kind="assert",
            site="kernel/lock.c:42",
            msg="mutex corrupted",
            boot_index=2,
            raw={"id": 3},
        ),
    ]

    # Ambiguous findings
    f_amb = [
        NC.FindingRecord(
            sig="sig_amb_1",
            tier="ambiguous",
            kind="livelock",
            site="kernel/sched.c:100",
            msg="temporary stall",
            boot_index=0,
            raw={"id": 10},
        ),
        NC.FindingRecord(
            sig="sig_amb_1",
            tier="ambiguous",
            kind="livelock",
            site="kernel/sched.c:100",
            msg="temporary stall",
            boot_index=1,
            raw={"id": 20},
        ),
        NC.FindingRecord(
            sig="sig_amb_1",
            tier="ambiguous",
            kind="livelock",
            site="kernel/sched.c:100",
            msg="temporary stall",
            boot_index=2,
            raw={"id": 30},
        ),
    ]

    for f in f_conf:
        ledger.record(f.boot_index, [f])
    for f in f_amb:
        ledger.record(f.boot_index, [f])

    reports = {r.sig: r for r in ledger.report()}

    conf = reports["sig_conf_1"]
    assert conf.occurrences == 3
    assert conf.repro_count == 3
    assert conf.repro_boots == [0, 1, 2]
    # Capped at confident_cap=2
    assert len(conf.evidence) == 2

    amb = reports["sig_amb_1"]
    assert amb.occurrences == 3
    assert amb.repro_count == 3
    assert amb.repro_boots == [0, 1, 2]
    # Ambiguous retains all 3
    assert len(amb.evidence) == 3


def test_progress_accumulator_monotonic_trace():
    acc = NC.ProgressAccumulator()

    # First boot: start = 0ms, stats = 100ms: 50, 200ms: 100, final: 120
    acc.record_boot(
        boot_index=0,
        task_name="task_a",
        boot_start_at_ms=0,
        stat_samples=[(100, 50), (200, 100)],
        final_progress=120,
    )

    # Second boot: start = 300ms, stats = 100ms: 80, 200ms: 150, final: 150
    acc.record_boot(
        boot_index=1,
        task_name="task_b",
        boot_start_at_ms=300,
        stat_samples=[(100, 80), (200, 150)],
        final_progress=150,
    )

    trace = acc.get_trace()

    # monotonicity verification
    progress_values = [t.cumulative_progress for t in trace]
    assert progress_values == sorted(progress_values)
    assert acc.total_progress == 120 + 150

    # boundary marker verification
    boundaries = [t for t in trace if t.is_boundary]
    assert len(boundaries) == 2
    assert boundaries[0].boot_index == 0 and boundaries[0].cumulative_progress == 0
    assert boundaries[1].boot_index == 1 and boundaries[1].cumulative_progress == 120


def test_campaign_runner_happy_path(tmp_path):
    suite = create_sample_suite()
    manifest = NC.CampaignManifest(
        suite=suite,
        runner_index=0,
        total_runners=1,
        out_dir=tmp_path / "results",
        gate_first=False,
    )

    mock_runner = MockBootRunner(
        [
            NC.BootResult(
                boot_index=0,
                task_name="task_a",
                cmdline="nightmare=task_a",
                seed=None,
                duration_ms=500,
                exit_code=0,
                status=NC.BootStatus.OK.value,
                reason="completed",
                progress=1000,
                findings=[],
            ),
            NC.BootResult(
                boot_index=1,
                task_name="task_a",
                cmdline="nightmare=task_a",
                seed=None,
                duration_ms=500,
                exit_code=0,
                status=NC.BootStatus.FINDING.value,
                reason="completed",
                progress=1000,
                findings=[
                    NC.FindingRecord(
                        sig="0xdeadbeef",
                        tier="confident",
                        kind="fault",
                        site="test.c:1",
                        msg="lock held",
                        boot_index=1,
                    )
                ],
            ),
            NC.BootResult(
                boot_index=2,
                task_name="task_a",
                cmdline="nightmare=task_a",
                seed=None,
                duration_ms=500,
                exit_code=0,
                status=NC.BootStatus.OK.value,
                reason="completed",
                progress=1000,
                findings=[],
            ),
        ]
    )

    result = NC.execute(manifest, boot_runner=mock_runner)
    assert result.status == NC.CampaignStatus.COMPLETED.value
    assert result.ok is True
    assert result.total_boots == 3
    assert result.completed_boots == 3
    assert result.finding_boots == 1
    assert len(result.findings) == 1
    assert result.findings[0].sig == "0xdeadbeef"

    json_text = NC.render_json(result)
    data = json.loads(json_text)
    assert data["summary"]["total_boots"] == 3
    assert data["summary"]["finding_boots"] == 1

    md_text = NC.render_markdown(result)
    assert "0xdeadbeef" in md_text


def test_campaign_runner_gate_first_failure(tmp_path):
    suite = create_sample_suite()
    manifest = NC.CampaignManifest(
        suite=suite,
        runner_index=0,
        total_runners=1,
        out_dir=tmp_path / "results",
        gate_first=True,
    )

    # gate boot fails
    mock_runner = MockBootRunner(
        [
            NC.BootResult(
                boot_index=0,
                task_name="gate",
                cmdline="",
                seed=None,
                duration_ms=200,
                exit_code=1,
                status=NC.BootStatus.FAIL.value,
                reason="test_failure",
                progress=0,
                findings=[
                    NC.FindingRecord(
                        sig="0xshould_be_ignored",
                        tier="confident",
                        kind="fail",
                        site="gate.c:1",
                        msg="gate broke",
                        boot_index=0,
                    )
                ],
            ),
        ]
    )

    result = NC.execute(manifest, boot_runner=mock_runner)
    assert result.status == NC.CampaignStatus.INFRASTRUCTURE.value
    assert result.ok is False
    # infra failures emit ZERO findings
    assert len(result.findings) == 0
    assert result.total_boots == 1
    assert result.failed_boots == 1
    assert mock_runner.call_count == 1


def test_cli_parser_nightmare_run():
    parser = build_parser()
    args = parser.parse_args(
        [
            "nightmare",
            "run",
            "nightmare/suites/harness_smoke.toml",
            "--runner",
            "1",
            "--total-runners",
            "4",
            "--seed",
            "0x1234",
            "--budget-hours",
            "0.5",
            "--dry-run",
        ]
    )
    assert args.command == "run"
    assert args.suite == "nightmare/suites/harness_smoke.toml"
    assert args.runner == 1
    assert args.total_runners == 4
    assert args.seed == "0x1234"
    assert args.budget_hours == 0.5
    assert args.dry_run is True
