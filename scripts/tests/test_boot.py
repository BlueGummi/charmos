"""charm boot: budgets, evidence collection, and stopping at the first failure"""

import json
import os
import time
from pathlib import Path

import pytest

from charm import boot as B
from charm import record as R


def test_a_timeout_kills_the_whole_process_group(tmp_path: Path) -> None:
    pidfile = tmp_path / "child.pid"
    started = time.monotonic()

    status = B._run(["sh", "-c", f"sleep 60 & echo $! > {pidfile}; wait"], 1, True)

    assert status == R.EXIT_TIMEOUT
    assert time.monotonic() - started < 15
    child = int(pidfile.read_text())
    with pytest.raises(ProcessLookupError):
        os.kill(child, 0)


def fake_build(tmp_path: Path, statuses: list[int], monkeypatch) -> Path:
    build = tmp_path / "build"
    build.mkdir()
    (build / "output.log").write_text("serial\n", encoding="utf-8")
    (build / "ndjson.log").write_text("", encoding="utf-8")
    calls = iter(statuses)
    monkeypatch.setattr(B, "_run", lambda *_args: next(calls))
    return build


def run_records(path: Path) -> dict:
    return next(
        r for r in map(json.loads, path.read_text().splitlines()) if r["type"] == "run"
    )


def test_each_boot_leaves_logs_and_records(tmp_path: Path, monkeypatch) -> None:
    build = fake_build(tmp_path, [0, 0], monkeypatch)
    out = tmp_path / "out"

    status = B.repeat(
        build_dir=build,
        out_dir=out,
        runs=2,
        timeout_s=5,
        meta=R.RunMeta(shard="gcc", scenario="gate"),
    )

    assert status == 0
    for name in ("gcc-1", "gcc-2"):
        assert (out / name / "console.log").read_text() == "serial\n"
        run = run_records(out / f"{name}.ndjson")
        assert run["shard"] == name
        assert run["scenario"] == "gate"
        assert run["exit_code"] == 0


def test_the_first_failure_stops_the_loop(tmp_path: Path, monkeypatch) -> None:
    build = fake_build(tmp_path, [0, 3, 0], monkeypatch)
    out = tmp_path / "out"

    status = B.repeat(
        build_dir=build, out_dir=out, runs=3, timeout_s=5, meta=R.RunMeta()
    )

    assert status == 3
    assert run_records(out / "run-2.ndjson")["outcome"] == "crash"
    assert not (out / "run-3.ndjson").exists()
