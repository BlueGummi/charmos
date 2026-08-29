#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Scenario:
    name: str
    cmdline: str
    status: int
    required: tuple[str, ...]
    forbidden: tuple[str, ...] = ()
    canary: bool = False
    repetitions: int = 1


BASE = (
    "nightmare=locks_storm nightmare.seed_mode=seedless "
    "nightmare.stat_interval_ms=50ms nightmare.on_stall=report "
)

NORMAL_REQUIRED = (
    '"test":"locks_storm"',
    '"caps":"preempt,lock_chk"',
    '"result":"ok","reason":"completed"',
    '"code":0',
)

NO_FINDING = ('"s":"nightmare","k":"finding"', '"s":"lock_chk"')

SCENARIOS = (
    Scenario(
        "normal",
        BASE + "nightmare.intensity=0.25 nightmare.duration_ms=350ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=1s",
        0,
        NORMAL_REQUIRED,
        NO_FINDING,
        repetitions=3,
    ),
    Scenario(
        "migration_wake",
        BASE + "nightmare.intensity=0.5 nightmare.duration_ms=450ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=1s "
        "nightmare.perturb=migrator,waker "
        "nightmare.perturb.migrator.interval_us=25us "
        "nightmare.perturb.waker.interval_us=25us",
        0,
        NORMAL_REQUIRED,
        NO_FINDING,
    ),
    Scenario(
        "long_stutter",
        BASE + "nightmare.intensity=0.25 nightmare.duration_ms=800ms "
        "nightmare.drain_grace_ms=500ms nightmare.stall_threshold_ms=100ms "
        "nightmare.locks_storm.worker_stall_ms=600ms "
        "nightmare.locks_storm.park_delay_ms=250ms "
        "nightmare.perturb=stutter "
        "nightmare.perturb.stutter.period_ms=100ms "
        "nightmare.perturb.stutter.gap_ms=400ms",
        0,
        (*NORMAL_REQUIRED, '"s":"nightmare","k":"quiesce"'),
        NO_FINDING,
    ),
    Scenario(
        "apc_pressure",
        BASE + "nightmare.intensity=0.5 nightmare.duration_ms=450ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=1s "
        "nightmare.perturb=apc_spammer "
        "nightmare.perturb.apc_spammer.interval_us=50us",
        0,
        NORMAL_REQUIRED,
        NO_FINDING,
    ),
    Scenario(
        "high_contention",
        BASE + "nightmare.intensity=1.0 nightmare.duration_ms=500ms "
        "nightmare.drain_grace_ms=500ms nightmare.stall_threshold_ms=200ms "
        "nightmare.locks_storm.worker_stall_ms=2s",
        0,
        NORMAL_REQUIRED,
        NO_FINDING,
        repetitions=3,
    ),
    Scenario(
        "canary_1",
        BASE + "nightmare.intensity=0.25 nightmare.duration_ms=800ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=1s "
        "nightmare.locks_storm.corrupt_after_ops=100",
        3,
        (
            '"kind":"lock_invariant","tier":"confident"',
            '"result":"finding","reason":"completed"',
            '"code":3',
        ),
        canary=True,
    ),
    Scenario(
        "canary_2",
        BASE + "nightmare.intensity=0.5 nightmare.duration_ms=800ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=1s "
        "nightmare.locks_storm.corrupt_after_ops=100",
        3,
        (
            '"kind":"lock_invariant","tier":"confident"',
            '"result":"finding","reason":"completed"',
            '"code":3',
        ),
        canary=True,
    ),
    Scenario(
        "canary_3",
        BASE + "nightmare.intensity=1.0 nightmare.duration_ms=800ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=1s "
        "nightmare.locks_storm.corrupt_after_ops=100",
        3,
        (
            '"kind":"lock_invariant","tier":"confident"',
            '"result":"finding","reason":"completed"',
            '"code":3',
        ),
        canary=True,
    ),
    Scenario(
        "worker_starvation",
        BASE + "nightmare.intensity=0.25 nightmare.duration_ms=800ms "
        "nightmare.drain_grace_ms=400ms nightmare.stall_threshold_ms=150ms "
        "nightmare.locks_storm.worker_stall_ms=100ms "
        "nightmare.locks_storm.starve_one=true",
        3,
        (
            '"kind":"worker_starvation","tier":"ambiguous"',
            '"result":"finding","reason":"completed"',
            '"code":3',
        ),
    ),
)


def records(path: Path) -> list[dict[str, object]]:
    parsed: list[dict[str, object]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            parsed.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return parsed


def run_scenario(
    scenario: Scenario, build_dir: Path, cmdline_dir: Path, configure: bool
) -> tuple[list[str], str | None]:
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
                "-DTEST_NIGHTMARE_ALL=OFF",
                "-DTEST_NIGHTMARE_LOCKS=ON",
                "-DDEBUG_LOCK_CHK=ON",
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
    if not ndjson_path.exists():
        return [*errors, "kernel NDJSON log was not produced"], None

    text = ndjson_path.read_text(encoding="utf-8")
    errors.extend(
        f"missing NDJSON: {item}" for item in scenario.required if item not in text
    )
    errors.extend(
        f"forbidden NDJSON present: {item}"
        for item in scenario.forbidden
        if item in text
    )

    progress = [
        int(record["progress"])
        for record in records(ndjson_path)
        if record.get("s") == "nightmare" and record.get("k") == "stat"
    ]
    if not progress or max(progress) == 0:
        errors.append("no non-zero nightmare progress sample")
    if progress != sorted(progress):
        errors.append("nightmare progress samples are not monotonic")

    canary_sig = None
    if scenario.canary:
        findings = [
            record
            for record in records(ndjson_path)
            if record.get("s") == "nightmare"
            and record.get("k") == "finding"
            and record.get("kind") == "lock_invariant"
        ]
        if len(findings) != 1:
            errors.append(f"expected one canary finding, got {len(findings)}")
        elif isinstance(findings[0].get("sig"), str):
            canary_sig = str(findings[0]["sig"])
        else:
            errors.append("canary finding has no string signature")

    return errors, canary_sig


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=REPO_ROOT / "build-locks-e2e",
        help="ignored build directory used for the kernel boots",
    )
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()

    failed = False
    canary_sigs: list[str] = []
    with tempfile.TemporaryDirectory(prefix="charmos-locks-") as tmp:
        cmdline_dir = Path(tmp)
        invocation = 0
        for scenario in SCENARIOS:
            for repetition in range(scenario.repetitions):
                errors, canary_sig = run_scenario(
                    scenario, build_dir, cmdline_dir, invocation == 0
                )
                invocation += 1
                if canary_sig is not None:
                    canary_sigs.append(canary_sig)
                suffix = (
                    f"[{repetition + 1}/{scenario.repetitions}]"
                    if scenario.repetitions > 1
                    else ""
                )
                label = f"{scenario.name}{suffix}"
                if errors:
                    failed = True
                    print(f"FAIL {label}")
                    for error in errors:
                        print(f"  {error}")
                else:
                    print(f"PASS {label}")

    if len(canary_sigs) != 3 or len(set(canary_sigs)) != 1:
        failed = True
        print(f"FAIL canary_dedup: signatures={canary_sigs}")
    else:
        print(f"PASS canary_dedup: signature={canary_sigs[0]} count=3")

    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
