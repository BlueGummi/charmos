import json
import shutil
import subprocess
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from datetime import UTC, datetime
from enum import StrEnum
from pathlib import Path
from typing import Any, ClassVar, Protocol

from .. import protocol as P
from .. import record as R
from . import codec
from .contracts import DiscoveryKind, ExecutionHealth
from .suite import FLUSH_MARGIN_MS, Suite, Task

CONFIDENT_FINDING_CAP = 5


class CampaignError(ValueError):
    pass


class CampaignStatus(StrEnum):
    COMPLETED = "completed"
    INFRASTRUCTURE = "infrastructure"
    BUDGET_EXHAUSTED = "budget_exhausted"
    ABORTED = "aborted"


class BootStatus(StrEnum):
    OK = "ok"
    FINDING = "finding"
    FAIL = "fail"
    STALL = "stall"
    SKIP = "skip"
    CRASH = "crash"
    TIMEOUT = "timeout"
    INFRA = "infra"
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class FindingRecord:
    sig: str
    tier: str
    kind: str
    site: str
    msg: str
    boot_index: int
    time_ms: int = 0
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass
class FindingSummary:
    sig: str
    tier: str
    kind: str
    site: str
    msg: str
    occurrences: int
    repro_boots: list[int]
    first_boot: int
    last_boot: int
    evidence: list[dict[str, Any]]

    @property
    def repro_count(self) -> int:
        return len(self.repro_boots)


@dataclass(frozen=True)
class TraceSample:
    at_ms: int
    cumulative_progress: int
    boot_index: int
    task_name: str
    is_boundary: bool = False


@dataclass
class BootResult:
    boot_index: int
    task_name: str
    cmdline: str
    seed: int | None
    duration_ms: int
    exit_code: int
    status: str
    reason: str
    progress: int
    findings: list[FindingRecord]
    stat_samples: list[tuple[int, int]] = field(
        default_factory=list
    )  # (rel_time_ms, progress)
    raw_records: list[dict[str, Any]] = field(default_factory=list)
    crashed: bool = False
    console_log: Path | None = None
    machine_log: Path | None = None

    @property
    def ok(self) -> bool:
        return self.status in (BootStatus.OK.value, BootStatus.FINDING.value)


@dataclass
class CampaignManifest:
    suite: Suite
    runner_index: int = 0
    total_runners: int = 1
    base_seed: int | None = None
    campaign_id: str = ""
    budget_ms: int = 0
    build_dir: Path = field(default_factory=lambda: Path("build"))
    out_dir: Path = field(default_factory=lambda: Path("campaign-results"))
    gate_first: bool | None = None  # None = use task default
    dry_run: bool = False

    def __post_init__(self) -> None:
        if not self.campaign_id:
            now_str = datetime.now(UTC).strftime("%Y%m%d-%H%M%S")
            object.__setattr__(self, "campaign_id", f"{self.suite.meta.name}-{now_str}")
        if self.budget_ms <= 0:
            object.__setattr__(
                self, "budget_ms", int(self.suite.meta.budget_hours * 3600 * 1000)
            )


@dataclass
class CampaignResult:
    campaign_id: str
    suite_name: str
    runner_index: int
    total_runners: int
    status: str
    ok: bool
    total_boots: int
    completed_boots: int
    finding_boots: int
    failed_boots: int
    stalled_boots: int
    skipped_boots: int
    total_progress: int
    total_duration_ms: int
    tail_unused_ms: int
    boots: list[BootResult]
    findings: list[FindingSummary]
    trace: list[TraceSample]

    @property
    def discovery_kind(self) -> DiscoveryKind:
        kinds: set[DiscoveryKind] = set()
        if self.findings:
            kinds.add(DiscoveryKind.FINDING)
        if any(boot.status == BootStatus.CRASH.value for boot in self.boots):
            kinds.add(DiscoveryKind.CRASH)
        if any(boot.status == BootStatus.STALL.value for boot in self.boots):
            kinds.add(DiscoveryKind.STALL)
        if not kinds:
            return DiscoveryKind.NONE
        if len(kinds) > 1:
            return DiscoveryKind.MIXED
        return next(iter(kinds))

    @property
    def execution_health(self) -> ExecutionHealth:
        if self.status == CampaignStatus.ABORTED.value:
            return ExecutionHealth.PARTIAL
        if self.status == CampaignStatus.INFRASTRUCTURE.value:
            return ExecutionHealth.INFRASTRUCTURE
        infrastructure_statuses = {
            BootStatus.FAIL.value,
            BootStatus.TIMEOUT.value,
            BootStatus.INFRA.value,
            BootStatus.UNKNOWN.value,
            BootStatus.SKIP.value,
        }
        if any(boot.status in infrastructure_statuses for boot in self.boots):
            return ExecutionHealth.INFRASTRUCTURE
        return ExecutionHealth.HEALTHY


class CampaignClock:
    """Tracks campaign budget and enforces intervals"""

    def __init__(
        self,
        budget_ms: int,
        time_fn: Callable[[], float] = time.monotonic,
        sleep_fn: Callable[[float], None] = time.sleep,
    ):
        self.budget_ms = max(0, budget_ms)
        self._time_fn = time_fn
        self._sleep_fn = sleep_fn
        self._start_time = self._time_fn()

    def elapsed_ms(self) -> int:
        return int((self._time_fn() - self._start_time) * 1000)

    def remaining_ms(self) -> int:
        rem = self.budget_ms - self.elapsed_ms()
        return max(0, rem)

    def can_fit_boot(
        self, boot_duration_ms: int, flush_margin_ms: int = FLUSH_MARGIN_MS
    ) -> bool:
        required = boot_duration_ms + flush_margin_ms
        return self.remaining_ms() >= required

    def enforce_min_interval(
        self, min_interval_ms: int, last_boot_started_at_s: float
    ) -> None:
        if min_interval_ms <= 0:
            return
        elapsed_s = self._time_fn() - last_boot_started_at_s
        target_s = min_interval_ms / 1000.0
        if elapsed_s < target_s:
            rem_s = target_s - elapsed_s
            rem_budget_s = self.remaining_ms() / 1000.0
            sleep_s = min(rem_s, rem_budget_s)
            if sleep_s > 0:
                self._sleep_fn(sleep_s)

    def tail_unused_ms(self) -> int:
        return self.remaining_ms()


class BootScheduler:
    """Plans boot sequence and partitions seeds across tasks/runners"""

    def __init__(
        self,
        tasks: Sequence[Task],
        runner_index: int = 0,
        total_runners: int = 1,
        base_seed: int | None = None,
    ):
        self.tasks = tuple(tasks)
        self.runner_index = runner_index
        self.total_runners = total_runners
        self.base_seed = base_seed
        self._schedule: list[tuple[Task, int, int | None]] = []
        self._build_schedule()

    def _build_schedule(self) -> None:
        if not self.tasks:
            return

        has_horizontal = any(t.mode == "horizontal" for t in self.tasks)

        if has_horizontal:
            task_remaining = {t.name: t.boot.max_boots for t in self.tasks}
            task_weights = {t.name: max(0.001, t.weight) for t in self.tasks}
            task_accum = {t.name: 0.0 for t in self.tasks}
            task_map = {t.name: t for t in self.tasks}

            boot_idx = 0
            while any(rem > 0 for rem in task_remaining.values()):
                candidates = [name for name, rem in task_remaining.items() if rem > 0]
                if not candidates:
                    break

                for name in candidates:
                    task_accum[name] += task_weights[name]

                chosen_name = max(candidates, key=lambda n: task_accum[n])
                task_accum[chosen_name] -= sum(
                    task_weights[n] for n in candidates
                ) / len(candidates)
                task_remaining[chosen_name] -= 1

                t = task_map[chosen_name]
                seed = None
                if t.nightmare.wants_seed:
                    seed = codec.seed_for(
                        self.base_seed or 0,
                        runner=self.runner_index,
                        boot_index=boot_idx,
                        boots_per_runner=t.boot.max_boots,
                    )
                self._schedule.append((t, boot_idx, seed))
                boot_idx += 1
        else:
            boot_idx = 0
            for t in self.tasks:
                for _ in range(t.boot.max_boots):
                    seed = None
                    if t.nightmare.wants_seed:
                        seed = codec.seed_for(
                            self.base_seed or 0,
                            runner=self.runner_index,
                            boot_index=boot_idx,
                            boots_per_runner=t.boot.max_boots,
                        )
                    self._schedule.append((t, boot_idx, seed))
                    boot_idx += 1

    def __iter__(self):
        return iter(self._schedule)

    def __len__(self) -> int:
        return len(self._schedule)


class SignatureLedger:
    """Collects and deduplicates findings"""

    def __init__(self, confident_cap: int = CONFIDENT_FINDING_CAP):
        self.confident_cap = confident_cap
        self._entries: dict[str, FindingSummary] = {}

    def record(self, boot_index: int, findings: Sequence[FindingRecord]) -> None:
        for f in findings:
            if f.sig not in self._entries:
                self._entries[f.sig] = FindingSummary(
                    sig=f.sig,
                    tier=f.tier,
                    kind=f.kind,
                    site=f.site,
                    msg=f.msg,
                    occurrences=1,
                    repro_boots=[boot_index],
                    first_boot=boot_index,
                    last_boot=boot_index,
                    evidence=[f.raw] if f.raw else [],
                )
            else:
                entry = self._entries[f.sig]
                entry.occurrences += 1
                entry.last_boot = boot_index
                if boot_index not in entry.repro_boots:
                    entry.repro_boots.append(boot_index)

                if entry.tier == "confident":
                    if len(entry.evidence) < self.confident_cap and f.raw:
                        entry.evidence.append(f.raw)
                else:
                    if f.raw:
                        entry.evidence.append(f.raw)

    def report(self) -> list[FindingSummary]:
        return sorted(self._entries.values(), key=lambda e: (-e.occurrences, e.sig))


class ProgressAccumulator:
    """Accumulates progress into a monotonic trace"""

    def __init__(self):
        self._trace: list[TraceSample] = []
        self._cumulative_offset = 0

    def record_boot(
        self,
        boot_index: int,
        task_name: str,
        boot_start_at_ms: int,
        stat_samples: Sequence[tuple[int, int]],
        final_progress: int,
    ) -> None:
        self._trace.append(
            TraceSample(
                at_ms=boot_start_at_ms,
                cumulative_progress=self._cumulative_offset,
                boot_index=boot_index,
                task_name=task_name,
                is_boundary=True,
            )
        )

        max_sample_progress = 0
        for rel_t_ms, progress in stat_samples:
            max_sample_progress = max(max_sample_progress, progress)
            self._trace.append(
                TraceSample(
                    at_ms=boot_start_at_ms + rel_t_ms,
                    cumulative_progress=self._cumulative_offset + progress,
                    boot_index=boot_index,
                    task_name=task_name,
                    is_boundary=False,
                )
            )

        effective_progress = max(final_progress, max_sample_progress)
        self._cumulative_offset += effective_progress

    def get_trace(self) -> list[TraceSample]:
        return list(self._trace)

    @property
    def total_progress(self) -> int:
        return self._cumulative_offset


class BootRunner(Protocol):
    """Interface for executing a single boot."""

    def run_boot(
        self,
        manifest: CampaignManifest,
        task: Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> BootResult: ...


def _result_from_logs(
    *,
    boot_index: int,
    task_name: str,
    cmdline: str,
    duration_ms: int,
    exit_code: int,
    timed_out: bool,
    console_log_path: Path,
    machine_log_path: Path,
) -> BootResult:
    raw_records = []
    stat_samples = []
    findings = []
    verdict_result = ""
    verdict_reason = ""
    final_progress = 0
    crashed = False

    if machine_log_path.is_file():
        for line in machine_log_path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                raw_records.append(rec)
            except json.JSONDecodeError:
                continue

            section = rec.get(P.KEY_SECTION)
            kind = rec.get(P.KEY_KIND)
            if section == P.SECTION_NIGHTMARE and kind == P.KIND_STAT:
                stat_samples.append((rec.get(P.KEY_TIME, 0), rec.get("progress", 0)))
            elif section == P.SECTION_NIGHTMARE and kind == P.KIND_FINDING:
                findings.append(
                    FindingRecord(
                        sig=rec.get("sig", "unknown"),
                        tier=rec.get("tier", "ambiguous"),
                        kind=rec.get("kind", "unknown"),
                        site=rec.get("site", ""),
                        msg=rec.get("msg", ""),
                        boot_index=boot_index,
                        time_ms=rec.get(P.KEY_TIME, 0),
                        raw=rec,
                    )
                )
            elif section == P.SECTION_NIGHTMARE and kind == P.KIND_VERDICT:
                verdict_result = rec.get("result", "")
                verdict_reason = rec.get("reason", "")
                final_progress = rec.get("progress", 0)
            elif section in (P.SECTION_PANIC, P.SECTION_ASAN):
                crashed = True

    if timed_out:
        status = BootStatus.TIMEOUT.value
        reason = "host_timeout"
    elif verdict_result:
        if verdict_result == "ok":
            status = BootStatus.FINDING.value if findings else BootStatus.OK.value
        elif verdict_result == "stall":
            status = BootStatus.STALL.value
        elif verdict_result == "skip":
            status = BootStatus.SKIP.value
        elif verdict_result == "fail":
            status = BootStatus.FAIL.value
        else:
            status = verdict_result
        reason = verdict_reason
    elif crashed or exit_code == R.EXIT_PANIC:
        status = BootStatus.CRASH.value
        reason = "kernel_crash"
    elif exit_code == 6:
        status = BootStatus.SKIP.value
        reason = "refusal"
    elif exit_code == 5:
        status = BootStatus.STALL.value
        reason = "stall"
    elif exit_code == 4:
        status = BootStatus.FAIL.value
        reason = "harness_fail"
    elif exit_code != 0:
        status = BootStatus.FAIL.value
        reason = f"exit_code_{exit_code}"
    else:
        status = BootStatus.OK.value
        reason = "exit_ok"

    return BootResult(
        boot_index=boot_index,
        task_name=task_name,
        cmdline=cmdline,
        seed=None,
        duration_ms=duration_ms,
        exit_code=exit_code,
        status=status,
        reason=reason,
        progress=final_progress,
        findings=findings,
        stat_samples=stat_samples,
        raw_records=raw_records,
        crashed=crashed,
        console_log=console_log_path,
        machine_log=machine_log_path,
    )


class QemuBootRunner:
    """Executes QEMU boots"""

    def run_boot(
        self,
        manifest: CampaignManifest,
        task: Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> BootResult:
        boot_dir = out_dir / f"boot-{boot_index:04d}"
        boot_dir.mkdir(parents=True, exist_ok=True)

        cmdline_file = boot_dir / "cmdline.txt"
        codec.write(cmdline_file, cmdline)

        console_log_path = boot_dir / "console.log"
        machine_log_path = boot_dir / "machine.nd.log"

        pristine_disk = manifest.build_dir / "d.img"
        runtime_disk = manifest.build_dir / "disk.img"
        if pristine_disk.is_file():
            shutil.copy2(pristine_disk, runtime_disk)

        cmd = [
            "./scripts/build.sh",
            "-B",
            str(manifest.build_dir),
            "-t",
            manifest.suite.build.type,
            "--compiler",
            manifest.suite.build.compiler,
            "--cmdline",
            str(cmdline_file.resolve()),
            "tests",
            "--",
            *codec.build_args(manifest.suite),
            "-DQEMU_NUMA=OFF"
            if manifest.suite.build.smp.total < 4
            else "-DQEMU_NUMA=ON",
        ]

        timeout_s = max(5.0, timeout_ms / 1000.0)
        start_time = time.monotonic()
        exit_code = 0
        timed_out = False

        try:
            proc = subprocess.run(
                cmd,
                cwd=manifest.build_dir.parent,
                capture_output=True,
                text=True,
                timeout=timeout_s,
            )
            exit_code = proc.returncode
            console_log_path.write_text(proc.stdout + proc.stderr, encoding="utf-8")
        except subprocess.TimeoutExpired as e:
            timed_out = True
            exit_code = R.EXIT_TIMEOUT
            out = (
                (e.stdout or "")
                if isinstance(e.stdout, str)
                else (e.stdout or b"").decode("utf-8", "replace")
            )
            err = (
                (e.stderr or "")
                if isinstance(e.stderr, str)
                else (e.stderr or b"").decode("utf-8", "replace")
            )
            console_log_path.write_text(out + err, encoding="utf-8")
        except OSError as e:
            console_log_path.write_text(f"Execution failed: {e}\n", encoding="utf-8")
            exit_code = 127

        duration_ms = int((time.monotonic() - start_time) * 1000)

        build_ndjson = manifest.build_dir / "ndjson.log"
        if build_ndjson.is_file():
            shutil.copy2(build_ndjson, machine_log_path)
        else:
            machine_log_path.write_text("", encoding="utf-8")

        return _result_from_logs(
            boot_index=boot_index,
            task_name=task.name,
            cmdline=cmdline,
            duration_ms=duration_ms,
            exit_code=exit_code,
            timed_out=timed_out,
            console_log_path=console_log_path,
            machine_log_path=machine_log_path,
        )


class BundleBootRunner:
    """Repack and boot a verified compile-once bundle."""

    _DEBUG_EXIT: ClassVar[dict[int, int]] = {
        1: 0,
        3: 1,
        5: 3,
        7: 3,
        9: 4,
        11: 5,
        13: 6,
    }

    def __init__(self, bundle: Any, repo_root: Path):
        self.bundle = bundle
        self.repo_root = repo_root

    def run_boot(
        self,
        manifest: CampaignManifest,
        task: Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> BootResult:
        from . import build_bundle as bundle_model

        boot_dir = out_dir / f"boot-{boot_index:04d}"
        boot_dir.mkdir(parents=True, exist_ok=True)
        cmdline_path = boot_dir / "cmdline.txt"
        codec.write(cmdline_path, cmdline)
        repacked = bundle_model.repack(
            self.bundle,
            cmdline=cmdline_path,
            out_dir=boot_dir,
            repo_root=self.repo_root,
        )
        disk_path = boot_dir / "disk.img"
        shutil.copy2(self.bundle.pristine_disk, disk_path)
        console_log_path = boot_dir / "console.log"
        machine_log_path = boot_dir / "machine.nd.log"
        machine_log_path.unlink(missing_ok=True)
        qmp_socket = boot_dir / "qmp.sock"
        qmp_socket.unlink(missing_ok=True)
        command = bundle_model.qemu_command(
            self.bundle,
            iso_path=repacked.iso_path,
            disk_path=disk_path,
            machine_log=machine_log_path,
            trace_log=boot_dir / "trace.log",
            qmp_socket=qmp_socket,
        )

        started = time.monotonic()
        timed_out = False
        try:
            completed = subprocess.run(
                command,
                cwd=boot_dir,
                capture_output=True,
                text=True,
                timeout=max(5.0, timeout_ms / 1000.0),
            )
            exit_code = self._DEBUG_EXIT.get(completed.returncode, completed.returncode)
            console_log_path.write_text(
                completed.stdout + completed.stderr, encoding="utf-8"
            )
        except subprocess.TimeoutExpired as error:
            timed_out = True
            exit_code = R.EXIT_TIMEOUT
            stdout = (
                error.stdout
                if isinstance(error.stdout, str)
                else (error.stdout or b"").decode("utf-8", "replace")
            )
            stderr = (
                error.stderr
                if isinstance(error.stderr, str)
                else (error.stderr or b"").decode("utf-8", "replace")
            )
            console_log_path.write_text(stdout + stderr, encoding="utf-8")
        except OSError as error:
            exit_code = 127
            console_log_path.write_text(
                f"Execution failed: {error}\n", encoding="utf-8"
            )
        duration_ms = int((time.monotonic() - started) * 1000)
        if not machine_log_path.exists():
            machine_log_path.write_text("", encoding="utf-8")
        return _result_from_logs(
            boot_index=boot_index,
            task_name=task.name,
            cmdline=cmdline,
            duration_ms=duration_ms,
            exit_code=exit_code,
            timed_out=timed_out,
            console_log_path=console_log_path,
            machine_log_path=machine_log_path,
        )


class CampaignRunner:
    def __init__(
        self,
        manifest: CampaignManifest,
        boot_runner: BootRunner | None = None,
        clock: CampaignClock | None = None,
    ):
        self.manifest = manifest
        self.boot_runner = boot_runner or QemuBootRunner()
        if clock is not None:
            self.clock = clock
        elif manifest.dry_run:
            sim_time = 0.0

            def sim_now() -> float:
                return sim_time

            def sim_sleep(s: float) -> None:
                nonlocal sim_time
                sim_time += s

            self.clock = CampaignClock(
                manifest.budget_ms,
                time_fn=sim_now,
                sleep_fn=sim_sleep,
            )
        else:
            self.clock = CampaignClock(manifest.budget_ms)
        self.ledger = SignatureLedger()
        self.accumulator = ProgressAccumulator()

    def execute(self) -> CampaignResult:
        manifest = self.manifest
        manifest.out_dir.mkdir(parents=True, exist_ok=True)

        should_gate = (
            manifest.gate_first
            if manifest.gate_first is not None
            else any(t.boot.gate_first for t in manifest.suite.tasks)
        )

        if should_gate and not manifest.dry_run:
            gate_res = self._run_gate_boot()
            if not gate_res.ok:
                return CampaignResult(
                    campaign_id=manifest.campaign_id,
                    suite_name=manifest.suite.meta.name,
                    runner_index=manifest.runner_index,
                    total_runners=manifest.total_runners,
                    status=CampaignStatus.INFRASTRUCTURE.value,
                    ok=False,
                    total_boots=1,
                    completed_boots=0,
                    finding_boots=0,
                    failed_boots=1,
                    stalled_boots=0,
                    skipped_boots=0,
                    total_progress=0,
                    total_duration_ms=self.clock.elapsed_ms(),
                    tail_unused_ms=self.clock.tail_unused_ms(),
                    boots=[gate_res],
                    findings=[],
                    trace=[],
                )

        scheduler = BootScheduler(
            manifest.suite.tasks,
            runner_index=manifest.runner_index,
            total_runners=manifest.total_runners,
            base_seed=manifest.base_seed,
        )

        boots: list[BootResult] = []
        status = CampaignStatus.COMPLETED.value

        for task, boot_index, seed in scheduler:
            if not self.clock.can_fit_boot(task.boot.duration_ms, FLUSH_MARGIN_MS):
                status = CampaignStatus.BUDGET_EXHAUSTED.value
                break

            boot_req = codec.BootRequest(
                boot_index=boot_index,
                campaign_id=manifest.campaign_id,
                seed=seed,
            )
            cmdline = codec.render(task, boot_req)

            boot_start_at_ms = self.clock.elapsed_ms()
            boot_start_time_s = self.clock._time_fn()

            if manifest.dry_run:
                b_res = BootResult(
                    boot_index=boot_index,
                    task_name=task.name,
                    cmdline=cmdline,
                    seed=seed,
                    duration_ms=task.boot.duration_ms,
                    exit_code=0,
                    status=BootStatus.OK.value,
                    reason="dry_run",
                    progress=1000,
                    findings=[],
                )
                self.clock._sleep_fn(task.boot.duration_ms / 1000.0)
            else:
                b_res = self.boot_runner.run_boot(
                    manifest=manifest,
                    task=task,
                    boot_index=boot_index,
                    cmdline=cmdline,
                    timeout_ms=task.boot.host_timeout_ms,
                    out_dir=manifest.out_dir,
                )
                b_res.seed = seed

            boots.append(b_res)

            if b_res.findings:
                self.ledger.record(boot_index, b_res.findings)

            self.accumulator.record_boot(
                boot_index=boot_index,
                task_name=task.name,
                boot_start_at_ms=boot_start_at_ms,
                stat_samples=b_res.stat_samples,
                final_progress=b_res.progress,
            )

            self.clock.enforce_min_interval(
                task.boot.min_interval_ms, boot_start_time_s
            )

        completed = sum(
            1
            for b in boots
            if b.status in (BootStatus.OK.value, BootStatus.FINDING.value)
        )
        finding_count = sum(1 for b in boots if b.status == BootStatus.FINDING.value)
        failed = sum(
            1
            for b in boots
            if b.status
            in (
                BootStatus.FAIL.value,
                BootStatus.CRASH.value,
                BootStatus.TIMEOUT.value,
                BootStatus.INFRA.value,
            )
        )
        stalled = sum(1 for b in boots if b.status == BootStatus.STALL.value)
        skipped = sum(1 for b in boots if b.status == BootStatus.SKIP.value)

        campaign_ok = (
            status
            in (CampaignStatus.COMPLETED.value, CampaignStatus.BUDGET_EXHAUSTED.value)
        ) and (failed == 0 and stalled == 0)

        return CampaignResult(
            campaign_id=manifest.campaign_id,
            suite_name=manifest.suite.meta.name,
            runner_index=manifest.runner_index,
            total_runners=manifest.total_runners,
            status=status,
            ok=campaign_ok,
            total_boots=len(boots),
            completed_boots=completed,
            finding_boots=finding_count,
            failed_boots=failed,
            stalled_boots=stalled,
            skipped_boots=skipped,
            total_progress=self.accumulator.total_progress,
            total_duration_ms=self.clock.elapsed_ms(),
            tail_unused_ms=self.clock.tail_unused_ms(),
            boots=boots,
            findings=self.ledger.report(),
            trace=self.accumulator.get_trace(),
        )

    def _run_gate_boot(self) -> BootResult:
        gate_task = self.manifest.suite.tasks[0]
        gate_cmdline = codec.render_gate(gate_task)
        return self.boot_runner.run_boot(
            manifest=self.manifest,
            task=gate_task,
            boot_index=0,
            cmdline=gate_cmdline,
            timeout_ms=gate_task.boot.host_timeout_ms,
            out_dir=self.manifest.out_dir / "gate",
        )


def render_json(result: CampaignResult) -> str:
    data = {
        "campaign_id": result.campaign_id,
        "suite_name": result.suite_name,
        "runner_index": result.runner_index,
        "total_runners": result.total_runners,
        "status": result.status,
        "ok": result.ok,
        "discovery": {
            "kind": result.discovery_kind.value,
            "finding_count": sum(f.occurrences for f in result.findings),
        },
        "execution": {"health": result.execution_health.value},
        "summary": {
            "total_boots": result.total_boots,
            "completed_boots": result.completed_boots,
            "finding_boots": result.finding_boots,
            "failed_boots": result.failed_boots,
            "stalled_boots": result.stalled_boots,
            "skipped_boots": result.skipped_boots,
            "total_progress": result.total_progress,
            "total_duration_ms": result.total_duration_ms,
            "tail_unused_ms": result.tail_unused_ms,
            "unique_findings": len(result.findings),
        },
        "findings": [
            {
                "sig": f.sig,
                "tier": f.tier,
                "kind": f.kind,
                "site": f.site,
                "msg": f.msg,
                "occurrences": f.occurrences,
                "repro_boots": f.repro_boots,
                "first_boot": f.first_boot,
                "last_boot": f.last_boot,
                "evidence_count": len(f.evidence),
                "evidence": f.evidence,
            }
            for f in result.findings
        ],
        "boots": [
            {
                "boot_index": b.boot_index,
                "task_name": b.task_name,
                "seed": hex(b.seed) if b.seed is not None else None,
                "duration_ms": b.duration_ms,
                "exit_code": b.exit_code,
                "status": b.status,
                "reason": b.reason,
                "progress": b.progress,
                "findings_count": len(b.findings),
            }
            for b in result.boots
        ],
        "trace": [
            {
                "at_ms": t.at_ms,
                "cumulative_progress": t.cumulative_progress,
                "boot_index": t.boot_index,
                "task_name": t.task_name,
                "is_boundary": t.is_boundary,
            }
            for t in result.trace
        ],
    }
    return json.dumps(data, indent=2) + "\n"


def render_markdown(result: CampaignResult) -> str:
    lines = [
        f"# Nightmare Campaign Summary: {result.campaign_id}",
        "",
        f"- **Suite**: `{result.suite_name}`",
        f"- **Runner**: `{result.runner_index}` of `{result.total_runners}`",
        f"- **Status**: **{result.status.upper()}** (ok={result.ok})",
        f"- **Boots**: {result.total_boots} total ({result.completed_boots} completed, {result.finding_boots} with findings, {result.failed_boots} failed, {result.stalled_boots} stalled, {result.skipped_boots} skipped)",
        f"- **Cumulative Progress**: {result.total_progress:,} iterations",
        f"- **Duration**: {result.total_duration_ms / 1000.0:.2f}s (unused tail: {result.tail_unused_ms / 1000.0:.2f}s)",
        "",
    ]

    if result.findings:
        lines.extend(
            [
                "## Findings",
                "",
                "| Signature | Tier | Kind | Site | Occurrences | Boots | Message |",
                "|---|---|---|---|---|---|---|",
            ]
        )
        for f in result.findings:
            boots_str = ", ".join(str(b) for b in f.repro_boots[:10])
            if len(f.repro_boots) > 10:
                boots_str += f", ... (+{len(f.repro_boots) - 10} more)"
            lines.append(
                f"| `{f.sig}` | `{f.tier}` | `{f.kind}` | `{f.site}` | {f.occurrences} | {boots_str} | {f.msg} |"
            )
        lines.append("")
    else:
        lines.extend(["## Findings", "", "No findings detected in this campaign.", ""])

    lines.extend(
        [
            "## Boots",
            "",
            "| Boot | Task | Seed | Duration | Exit | Status | Progress | Findings |",
            "|---|---|---|---|---|---|---|---|",
        ]
    )
    for b in result.boots:
        seed_str = f"0x{b.seed:016x}" if b.seed is not None else "-"
        lines.append(
            f"| {b.boot_index} | {b.task_name} | {seed_str} | {b.duration_ms}ms | {b.exit_code} | `{b.status}` | {b.progress:,} | {len(b.findings)} |"
        )
    lines.append("")

    return "\n".join(lines)


def execute(
    manifest: CampaignManifest,
    boot_runner: BootRunner | None = None,
    clock: CampaignClock | None = None,
) -> CampaignResult:
    runner = CampaignRunner(manifest, boot_runner=boot_runner, clock=clock)
    return runner.execute()


def write_reports(result: CampaignResult, out_dir: Path) -> tuple[Path, Path]:
    """Write JSON and Markdown summaries"""
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "campaign_report.json"
    md_path = out_dir / "campaign_summary.md"

    json_path.write_text(render_json(result), encoding="utf-8")
    md_path.write_text(render_markdown(result), encoding="utf-8")
    return json_path, md_path
