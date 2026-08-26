import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

from . import record as R
from . import report as RP

COLLECT = {
    "output.log": "console.log",
    "output.stderr.log": "qemu.log",
    "ndjson.log": "machine.nd.log",
}


@dataclass
class Iteration:
    index: int
    status: int
    duration_s: float
    out_dir: Path

    @property
    def ok(self) -> bool:
        return self.status == 0


def collect(build_dir: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for src, dst in COLLECT.items():
        path = build_dir / src
        if path.is_file():
            shutil.copy2(path, out_dir / dst)


def run_once(build_dir: Path, target: str, timeout_s: int) -> tuple[int, float]:
    """Build the target once. Returns (status, wall seconds)."""
    argv = ["cmake", "--build", str(build_dir), "--target", target]
    start = time.monotonic()
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout_s)
        status = proc.returncode
    except subprocess.TimeoutExpired:
        status = R.EXIT_TIMEOUT
    except FileNotFoundError:
        raise SystemExit("charm: cmake is not on PATH") from None
    return status, time.monotonic() - start


def repeat(
    build_dir: Path,
    results_root: Path,
    runs: int,
    timeout_s: int,
    target: str,
) -> int:
    batch = datetime.now(UTC).strftime("%Y%m%d-%H%M%S")
    results_dir = results_root / batch
    print(f"charm: collecting into {results_dir}", file=sys.stderr)

    for index in range(1, runs + 1):
        out_dir = results_dir / f"run-{index}"
        print(f"[{index}/{runs}] {target}", file=sys.stderr, flush=True)

        status, duration = run_once(build_dir, target, timeout_s)
        collect(build_dir, out_dir)
        it = Iteration(index, status, duration, out_dir)

        if it.ok:
            print(f"[{index}/{runs}] PASS ({duration:.0f}s)", file=sys.stderr)
            continue

        label = "TIMEOUT" if status == R.EXIT_TIMEOUT else f"FAIL (status {status})"
        print(f"[{index}/{runs}] {label} after {duration:.0f}s", file=sys.stderr)

        machine = out_dir / COLLECT["ndjson.log"]
        console = out_dir / COLLECT["output.log"]
        if not machine.is_file():
            print(f"no machine log found at {machine}", file=sys.stderr)
            return status

        with machine.open(encoding="utf-8", errors="replace") as fh:
            result = R.parse_ndjson(fh)
        meta = R.RunMeta(
            shard=f"run-{index}",
            scenario="local",
            exit_code=status,
            duration_s=duration,
        )
        log_bytes = console.stat().st_size if console.is_file() else 0
        run = R.build_run_record(result, meta, log_bytes)

        records_path = out_dir / "results.ndjson"
        records_path.write_text(R.render([run, *result.records]), encoding="utf-8")

        runs_, tests, crashes, _ = RP.load([records_path])
        rep = RP.build_report(runs_, tests, crashes, "")
        sys.stderr.write(RP.render_markdown(rep))
        print(f"records: {records_path}", file=sys.stderr)
        return status

    print(f"{runs}/{runs} consecutive runs passed", file=sys.stderr)
    return 0
