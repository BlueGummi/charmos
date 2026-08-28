#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


@dataclass(frozen=True)
class Scenario:
    name: str
    cmdline: str
    status: int
    ndjson_required: tuple[str, ...]
    ndjson_forbidden: tuple[str, ...] = ()
    console_required: tuple[str, ...] = ()


BASE = (
    "nightmare=harness_smoke nightmare.seed_mode=seedless "
    "nightmare.intensity=0.5 nightmare.stat_interval_ms=50ms "
)

SCENARIOS = (
    Scenario(
        "normal",
        BASE + "nightmare.duration_ms=300ms nightmare.drain_grace_ms=300ms "
        "nightmare.stall_threshold_ms=100ms nightmare.on_stall=report",
        0,
        ('"result":"ok","reason":"completed"', '"code":0'),
        ('"d":"nightmare","k":"finding"',),
    ),
    Scenario(
        "plateau",
        BASE + "nightmare.duration_ms=600ms nightmare.drain_grace_ms=300ms "
        "nightmare.stall_threshold_ms=100ms nightmare.on_stall=report "
        "nightmare.harness_smoke.plateau=true",
        5,
        (
            '"d":"nightmare","k":"finding"',
            '"result":"stall","reason":"liveness"',
            '"code":5',
        ),
    ),
    Scenario(
        "blocked_drain",
        BASE + "nightmare.duration_ms=300ms nightmare.drain_grace_ms=200ms "
        "nightmare.stall_threshold_ms=100ms nightmare.on_stall=report "
        "nightmare.harness_smoke.blocked_drain=true",
        5,
        (
            '"d":"nightmare","k":"finding"',
            '"result":"stall","reason":"drain_timeout"',
            '"code":5',
        ),
    ),
    Scenario(
        "migration",
        BASE + "nightmare.duration_ms=400ms nightmare.drain_grace_ms=300ms "
        "nightmare.stall_threshold_ms=150ms nightmare.on_stall=report "
        "nightmare.perturb=migrator "
        "nightmare.perturb.migrator.interval_us=25us",
        0,
        ('"result":"ok","reason":"completed"', '"code":0'),
        ('"d":"nightmare","k":"finding"',),
    ),
    Scenario(
        "long_stutter",
        BASE + "nightmare.duration_ms=900ms nightmare.drain_grace_ms=400ms "
        "nightmare.stall_threshold_ms=100ms nightmare.on_stall=report "
        "nightmare.perturb=stutter "
        "nightmare.perturb.stutter.period_ms=100ms "
        "nightmare.perturb.stutter.gap_ms=500ms "
        "nightmare.harness_smoke.park_delay_ms=300ms",
        0,
        (
            '"d":"nightmare","k":"quiesce"',
            '"result":"ok","reason":"completed"',
            '"code":0',
        ),
        ('"d":"nightmare","k":"finding"',),
    ),
    Scenario(
        "single_worker_starvation",
        BASE + "nightmare.duration_ms=300ms nightmare.drain_grace_ms=300ms "
        "nightmare.stall_threshold_ms=100ms nightmare.on_stall=report "
        "nightmare.harness_smoke.starve_one=true",
        0,
        ('"result":"ok","reason":"completed"', '"code":0'),
        ('"d":"nightmare","k":"finding"',),
    ),
    Scenario(
        "crash_policy",
        BASE + "nightmare.duration_ms=600ms nightmare.drain_grace_ms=300ms "
        "nightmare.stall_threshold_ms=100ms nightmare.on_stall=crash "
        "nightmare.harness_smoke.plateau=true",
        3,
        (
            '"d":"nightmare","k":"finding"',
            '"d":"panic","k":"at"',
            '"code":2,"reason":"crash"',
        ),
        console_required=("other CPUs", "cpu 1"),
    ),
)


def contains_all(text: str, needles: tuple[str, ...]) -> list[str]:
    return [needle for needle in needles if needle not in text]


def run_scenario(
    scenario: Scenario, build_dir: Path, cmdline_dir: Path, configure: bool
) -> list[str]:
    cmdline_path = cmdline_dir / f"{scenario.name}.txt"
    cmdline_path.write_text(scenario.cmdline + "\n", encoding="utf-8")

    command = [
        str(REPO_ROOT / "scripts" / "build.sh"),
        "-q",
        "-B",
        str(build_dir),
        "--cmdline",
        str(cmdline_path),
        "tests",
    ]
    if configure:
        command.extend(
            (
                "--",
                "-DTEST_ALL=ON",
                "-DTEST_NIGHTMARE_ALL=ON",
                "-DQEMU_SMP_TOPO=sockets=1,cores=2,threads=1",
                "-DQEMU_NUMA=OFF",
                "-DQEMU_MEM_SIZE=512M",
            )
        )

    result = subprocess.run(command, cwd=REPO_ROOT, check=False)
    errors: list[str] = []
    if result.returncode != scenario.status:
        errors.append(f"exit status {result.returncode}, expected {scenario.status}")
    ndjson_path = build_dir / "ndjson.log"
    console_path = build_dir / "output.log"
    if not ndjson_path.exists() or not console_path.exists():
        errors.append("kernel logs were not produced")
        return errors

    ndjson = ndjson_path.read_text(encoding="utf-8")
    console = ANSI_ESCAPE.sub("", console_path.read_text(encoding="utf-8"))
    errors.extend(
        f"missing NDJSON: {needle}"
        for needle in contains_all(ndjson, scenario.ndjson_required)
    )
    errors.extend(
        f"forbidden NDJSON present: {needle}"
        for needle in scenario.ndjson_forbidden
        if needle in ndjson
    )
    errors.extend(
        f"missing console evidence: {needle}"
        for needle in contains_all(console, scenario.console_required)
    )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=REPO_ROOT / "build-liveness-e2e",
        help="ignored build directory used for the kernel boots",
    )
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()

    failed = False
    with tempfile.TemporaryDirectory(prefix="charmos-liveness-") as tmp:
        cmdline_dir = Path(tmp)
        for index, scenario in enumerate(SCENARIOS):
            errors = run_scenario(scenario, build_dir, cmdline_dir, index == 0)
            if errors:
                failed = True
                print(f"FAIL {scenario.name}")
                for error in errors:
                    print(f"  {error}")
            else:
                print(f"PASS {scenario.name}")

    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
