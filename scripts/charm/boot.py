"""Boot a build's test target and keep what it left behind

One loop for CI shards and local repro runs: each boot gets a wall budget,
its logs are collected, and its machine channel becomes result records
(`<name>.ndjson`) that `charm ci aggregate` reads.
"""

import os
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path

from . import record as R
from . import report as RP

COLLECT = {
    "output.log": "console.log",
    "output.stderr.log": "qemu.log",
    "ndjson.log": "machine.nd.log",
}
KILL_GRACE_S = 10
CONSOLE_TAIL = 40


@dataclass(frozen=True)
class Boot:
    name: str
    status: int
    duration_s: float
    records: Path


def _run(argv: list[str], timeout_s: int, quiet: bool) -> int:
    """Run argv in its own process group, so a timeout takes QEMU down too."""
    output = subprocess.DEVNULL if quiet else None
    proc = subprocess.Popen(argv, stdout=output, stderr=output, start_new_session=True)
    try:
        return proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        for sig, grace in ((signal.SIGTERM, KILL_GRACE_S), (signal.SIGKILL, None)):
            try:
                os.killpg(proc.pid, sig)
                proc.wait(timeout=grace)
                break
            except subprocess.TimeoutExpired:
                continue
            except ProcessLookupError:
                break
        return R.EXIT_TIMEOUT


def _records(out_dir: Path, meta: R.RunMeta) -> Path:
    console = out_dir / COLLECT["output.log"]
    machine = out_dir / COLLECT["ndjson.log"]
    with machine.open(encoding="utf-8", errors="replace") as fh:
        result = R.parse_ndjson(fh)
    result.records.extend(
        {"type": "schema_error", "detail": err} for err in result.stats.schema_errors
    )
    log_bytes = console.stat().st_size if console.is_file() else 0
    run = R.build_run_record(result, meta, log_bytes)
    path = out_dir.parent / f"{out_dir.name}.ndjson"
    path.write_text(R.render([run, *result.records]), encoding="utf-8")
    return path


def boot(
    build_dir: Path, out_dir: Path, meta: R.RunMeta, timeout_s: int, quiet: bool
) -> Boot:
    argv = ["cmake", "--build", str(build_dir), "--target", "tests"]
    start = time.monotonic()
    status = _run(argv, timeout_s, quiet)
    duration = time.monotonic() - start

    out_dir.mkdir(parents=True, exist_ok=True)
    for src, dst in COLLECT.items():
        if (build_dir / src).is_file():
            shutil.copy2(build_dir / src, out_dir / dst)
    machine = out_dir / COLLECT["ndjson.log"]
    if not machine.is_file():
        machine.write_text("", encoding="utf-8")

    meta = replace(meta, exit_code=status, duration_s=round(duration, 1))
    return Boot(meta.shard or out_dir.name, status, duration, _records(out_dir, meta))


def repeat(
    *,
    build_dir: Path,
    out_dir: Path,
    runs: int,
    timeout_s: int,
    meta: R.RunMeta,
    quiet: bool = False,
) -> int:
    """Boot up to `runs` times, stopping at the first failure."""
    ci = os.environ.get("GITHUB_ACTIONS") == "true"
    base = meta.shard or "run"
    for index in range(1, runs + 1):
        name = base if runs == 1 else f"{base}-{index}"
        label = f"[{index}/{runs}] {name}"
        print(f"::group::{label}" if ci else label, file=sys.stderr, flush=True)
        result = boot(
            build_dir, out_dir / name, replace(meta, shard=name), timeout_s, quiet
        )
        if ci:
            print("::endgroup::", file=sys.stderr)

        if result.status == 0:
            print(f"{label} PASS ({result.duration_s:.0f}s)", file=sys.stderr)
            continue

        verdict = (
            "TIMEOUT"
            if result.status == R.EXIT_TIMEOUT
            else f"FAIL (status {result.status})"
        )
        print(f"{label} {verdict} after {result.duration_s:.0f}s", file=sys.stderr)
        console = out_dir / name / COLLECT["output.log"]
        if console.is_file():
            lines = console.read_text(encoding="utf-8", errors="replace").splitlines()
            sys.stderr.write("\n".join(lines[-CONSOLE_TAIL:]) + "\n")
        runs_, tests, crashes, _ = RP.load([result.records])
        sys.stderr.write(RP.render_markdown(RP.build_report(runs_, tests, crashes, "")))
        print(f"records: {result.records}", file=sys.stderr)
        return result.status

    print(f"{runs}/{runs} consecutive boots passed", file=sys.stderr)
    return 0
