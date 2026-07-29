#!/usr/bin/env python3
"""Turn a QEMU serial log into NDJSON result records

The kernel prints for humans, so NOTE: we must update this when outputs change.

Why NDJSON?

a run killed by timeout or triple fault never gets a closing brace, and truncated JSON
is unparseable, this avoids this problem

Usage:
    parse_log.py output.log --shard 3 --seed 0xabc --out results.ndjson
"""

import argparse
import hashlib
import json
import re
import sys

SCHEMA_VERSION = 1

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")

# run_qemu.sh folds stderr into guest serial, and QEMU writes whenever it wants,
# so we have to parse this out
HOST_NOISE_RE = re.compile(r"qemu-system-[^\s:]*:[^\n]*\n?")

# "[8.909] " timestamps 
STAMP_RE = re.compile(r"^\[(\d+)\.(\d{3})\]\s*")

# "[8.909] tmpfs (integration): tmpfs_rw_test successful in 1 ms (reason: none)"
TEST_RE = re.compile(
    r"^\[(?P<sec>\d+)\.(?P<ms>\d{3})\]\s+"
    r"(?P<group>\S+)\s+\((?P<tier>\w+)\):\s+"
    r"(?P<name>\S+)\s+"
    r"(?P<status>successful|skipped|error)\s+in\s+(?P<dur>\d+)\s+ms"
    r"(?:\s+\(reason:\s*(?P<reason>[^)]*)\))?"
    r"(?:\s+\((?P<message>.*?)\))?\s*$"
)

# Multi-run form: " ran (97/100) times in 812 ms, 3 failed"
MULTI_RE = re.compile(
    r"^\[(?P<sec>\d+)\.(?P<ms>\d{3})\]\s+"
    r"(?P<group>\S+)\s+\((?P<tier>\w+)\):\s+"
    r"(?P<name>\S+)\s+ran\s+\((?P<ran>\d+)/(?P<total>\d+)\)\s+times\s+in\s+"
    r"(?P<dur>\d+)\s+ms,\s*(?P<tail>.*?)\s*$"
)

MULTI_FAILED_RE = re.compile(r"(\d+)\s+failed")
MULTI_SKIPPED_RE = re.compile(r"(\d+)\s+skipped")

# "[0.833] test_harness: Running group apc - 2 tests in (kernel/tests/apc.c)"
GROUP_START_RE = re.compile(
    r"test_harness:\s+Running\s+group\s+(?P<group>\S+)\s+-\s+"
    r"(?P<count>\d+)\s+tests\s+in\s+\((?P<file>[^)]+)\)"
)

# "[0.846] test_harness: Test group apc successful in 2 ms"
# "[8.887] test_harness: Test group sync_nightmare completed in 0 ms, 1 skipped"
GROUP_END_RE = re.compile(
    r"test_harness:\s+Test\s+group\s+(?P<group>\S+)\s+"
    r"(?:successful|completed)\s+in\s+(?P<dur>\d+)\s+ms(?P<tail>.*)$"
)

# "[8.918] test_harness: 91 total tests, 89 passed, 0 failed, 2 skipped"
TOTALS_RE = re.compile(
    r"test_harness:\s+(?P<total>\d+)\s+total\s+tests,\s+"
    r"(?P<passed>\d+)\s+passed,\s+(?P<failed>\d+)\s+failed,\s+"
    r"(?P<skipped>\d+)\s+skipped"
)

# "[8.923] test_harness: all tests pass 🎉! (7461 ms)"
VERDICT_RE = re.compile(
    r"test_harness:\s+(?P<msg>all tests pass.*?|some errors occurred)\s+"
    r"\((?P<dur>\d+)\s+ms\)"
)

# "[0.832] test_harness: Running 91 tests:"
RUNNING_RE = re.compile(r"test_harness:\s+Running\s+(?P<count>\d+)\s+tests:")

PANIC_MARKER = "[KERNEL PANIC]"
PANIC_FROM_RE = re.compile(r"\[FROM\]\s+(?P<site>\S+)")
PANIC_MSG_RE = re.compile(r"\[MESSAGE\]\s*(?P<msg>.*)$")

# "    #0  [0xffffffff80051de7] tests_run+0x1037"
FRAME_RE = re.compile(
    r"#\s*(?P<idx>\d+)\s+\[0x(?P<addr>[0-9a-fA-F]+)\]\s+"
    r"(?P<sym>\S+?)(?:\+0x(?P<off>[0-9a-fA-F]+))?\s*$"
)

# "[ASAN] ASAN: invalid memory access (poisoned) at 0xffff.. size=8 store"
ASAN_RE = re.compile(
    r"\[ASAN\]\s+(?P<what>.*?)\s+at\s+(?P<addr>0x[0-9a-fA-F]+)\s+"
    r"size=(?P<size>\d+)\s+(?P<access>load|store)"
)

# TEST_ASSERT: ' assert "x == y" failed at kernel/tests/mem.c:42 '
ASSERT_RE = re.compile(
    r'assert\s+"(?P<expr>.*?)"\s+failed\s+at\s+(?P<site>\S+):(?P<line>\d+)'
)

STATUS_MAP = {"successful": "pass", "skipped": "skip", "error": "fail"}

# Addresses and such
HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
NUM_RE = re.compile(r"\b\d+\b")

SIGNATURE_FRAMES = 5


def strip_ansi(text):
    return ANSI_RE.sub("", text)


def is_complete_record(line):
    return any(
        pat.match(line) if pat in (TEST_RE, MULTI_RE) else pat.search(line)
        for pat in (
            TEST_RE,
            MULTI_RE,
            GROUP_START_RE,
            GROUP_END_RE,
            TOTALS_RE,
            VERDICT_RE,
            RUNNING_RE,
        )
    )


def logical_lines(lines):
    """Rejoin console-wrapped records

    The console wraps at width, so a harness record can be several physical lines

    Here, we use the timestamp to discover where a newline logically starts

    panic and ASAN carry no timestamps, and follow whatever was last printed,
    so records that are already complete are never extended
    """
    out = []
    for line in lines:
        if STAMP_RE.match(line) or not out or is_complete_record(out[-1]):
            out.append(line.rstrip())
        else:
            out[-1] = out[-1] + " " + line.strip()
    return out


def normalize_for_signature(text):
    text = HEX_RE.sub("<addr>", text)
    return NUM_RE.sub("<n>", text)


def crash_signature(kind, site, message, frames):
    """Stable signature for a crash, so N shards with one bug report once

    Does not include addresses, offsets, shards, only keeping frame names and such
    """
    parts = [kind, site or "", normalize_for_signature(message or "")]
    parts.extend(f.get("symbol", "?") for f in frames[:SIGNATURE_FRAMES])
    digest = hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()
    return digest[:12]


def parse_frames(lines, start):
    """ get backtrace frames starting at `start` """
    frames = []
    for line in lines[start:]:
        m = FRAME_RE.search(line)
        if not m:
            # Blank lines and the banner sit between frames and whatever
            # follows, and anything else means the backtrace is over
            if line.strip() == "" or set(line.strip()) == {"="}:
                if frames:
                    break
                continue
            if frames:
                break
            continue
        frames.append(
            {
                "index": int(m.group("idx")),
                "address": "0x" + m.group("addr"),
                "symbol": m.group("sym"),
                "offset": int(m.group("off"), 16) if m.group("off") else 0,
            }
        )
    return frames


def parse_crashes(lines):
    """Second pass over raw lines for panics, ASAN reports, and failed assertions
    
       Kept separate since crash output has no timestamps
    """
    crashes = []

    for i, line in enumerate(lines):
        if PANIC_MARKER in line:
            site, func, message = None, None, None
            for probe in lines[i : i + 40]:
                if site is None:
                    m = PANIC_FROM_RE.search(probe)
                    if m:
                        site = m.group("site")
                if message is None:
                    m = PANIC_MSG_RE.search(probe)
                    if m:
                        message = m.group("msg").strip()
            frames = parse_frames(lines, i)
            if site and site.count(":") >= 2:
                path, lineno, func = site.split(":", 2)
                site = f"{path}:{lineno}"
                func = func.rstrip("()")
            crashes.append(
                {
                    "kind": "panic",
                    "site": site,
                    "function": func,
                    "message": message,
                    "frames": frames,
                }
            )
            continue

        m = ASAN_RE.search(line)
        if m:
            frames = parse_frames(lines, i + 1)
            crashes.append(
                {
                    "kind": "asan",
                    "site": None,
                    "function": None,
                    "message": "%s (%s, size %s)"
                    % (m.group("what"), m.group("access"), m.group("size")),
                    "address": m.group("addr"),
                    "frames": frames,
                }
            )
            continue

        m = ASSERT_RE.search(line)
        if m:
            crashes.append(
                {
                    "kind": "assert",
                    "site": "%s:%s" % (m.group("site"), m.group("line")),
                    "function": None,
                    "message": 'assert "%s" failed' % m.group("expr"),
                    "frames": [],
                }
            )

    for c in crashes:
        c["signature"] = crash_signature(
            c["kind"], c.get("site"), c.get("message"), c.get("frames", [])
        )
    return crashes


def parse(text):
    """Return (records, stats) for one serial log."""
    stripped = strip_ansi(text)
    stripped, host_noise = HOST_NOISE_RE.subn("", stripped)
    raw_lines = stripped.splitlines()
    lines = logical_lines(raw_lines)

    records = []
    stats = {
        "tests_seen": 0,
        "declared_total": None,
        "totals": None,
        "verdict": None,
        "reached_summary": False,
    }
    current_group = None

    for line in lines:
        m = RUNNING_RE.search(line)
        if m:
            stats["declared_total"] = int(m.group("count"))
            continue

        m = GROUP_START_RE.search(line)
        if m:
            current_group = m.group("group")
            records.append(
                {
                    "type": "group_start",
                    "group": m.group("group"),
                    "test_count": int(m.group("count")),
                    "file": m.group("file"),
                }
            )
            continue

        m = TEST_RE.match(line)
        if m:
            stats["tests_seen"] += 1
            rec = {
                "type": "test",
                "group": m.group("group"),
                "tier": m.group("tier"),
                "name": m.group("name"),
                "status": STATUS_MAP[m.group("status")],
                "duration_ms": int(m.group("dur")),
                "t_ms": int(m.group("sec")) * 1000 + int(m.group("ms")),
            }
            if m.group("reason") is not None:
                rec["reason"] = m.group("reason").strip()
            if m.group("message") is not None:
                rec["message"] = m.group("message").strip()
            records.append(rec)
            continue

        m = MULTI_RE.match(line)
        if m:
            stats["tests_seen"] += 1
            tail = m.group("tail")
            failed = MULTI_FAILED_RE.search(tail)
            skipped = MULTI_SKIPPED_RE.search(tail)
            failed_n = int(failed.group(1)) if failed else 0
            skipped_n = int(skipped.group(1)) if skipped else 0
            total_n = int(m.group("total"))
            records.append(
                {
                    "type": "test",
                    "group": m.group("group"),
                    "tier": m.group("tier"),
                    "name": m.group("name"),

                    # Tests passing 97 times and failing 3 are a separate state
                    "status": "flaky" if 0 < failed_n < total_n else (
                        "fail" if failed_n else "pass"
                    ),
                    "duration_ms": int(m.group("dur")),
                    "t_ms": int(m.group("sec")) * 1000 + int(m.group("ms")),
                    "runs": {
                        "attempted": int(m.group("ran")),
                        "requested": total_n,
                        "failed": failed_n,
                        "skipped": skipped_n,
                    },
                }
            )
            continue

        m = GROUP_END_RE.search(line)
        if m:
            tail = m.group("tail")
            failed = MULTI_FAILED_RE.search(tail)
            skipped = MULTI_SKIPPED_RE.search(tail)
            records.append(
                {
                    "type": "group_end",
                    "group": m.group("group"),
                    "duration_ms": int(m.group("dur")),
                    "failed": int(failed.group(1)) if failed else 0,
                    "skipped": int(skipped.group(1)) if skipped else 0,
                }
            )
            current_group = None
            continue

        m = TOTALS_RE.search(line)
        if m:
            stats["reached_summary"] = True
            stats["totals"] = {
                "total": int(m.group("total")),
                "passed": int(m.group("passed")),
                "failed": int(m.group("failed")),
                "skipped": int(m.group("skipped")),
            }
            records.append({"type": "totals", **stats["totals"]})
            continue

        m = VERDICT_RE.search(line)
        if m:
            stats["verdict"] = {
                "ok": m.group("msg").startswith("all tests pass"),
                "duration_ms": int(m.group("dur")),
            }
            records.append({"type": "verdict", **stats["verdict"]})

    for crash in parse_crashes(raw_lines):
        records.append({"type": "crash", **crash})

    stats["crashes"] = sum(1 for r in records if r["type"] == "crash")
    stats["last_group"] = current_group
    stats["host_noise_stripped"] = host_noise
    return records, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="QEMU serial log")
    ap.add_argument("--out", help="NDJSON output (default: stdout)")
    ap.add_argument("--shard", default=None)
    ap.add_argument("--scenario", default=None)
    ap.add_argument("--seed", default=None)
    ap.add_argument("--sha", default=None)
    ap.add_argument("--compiler", default=None)
    ap.add_argument("--config", default=None, help="free-form guest config label")
    ap.add_argument("--duration-s", type=float, default=None)
    ap.add_argument(
        "--exit-code",
        type=int,
        default=None,
        help="exit status of the QEMU wrapper (0 pass, 1 test failure, "
        "124 timeout by convention)",
    )
    args = ap.parse_args()

    with open(args.log, "rb") as fh:
        text = fh.read().decode("utf-8", "replace")

    records, stats = parse(text)

    # These are potentially dirty runs
    outcome = "pass"
    if args.exit_code is not None and args.exit_code == 124:
        outcome = "timeout"
    elif stats["crashes"]:
        outcome = "crash"
    elif not stats["reached_summary"]:
        outcome = "incomplete"
    elif stats["totals"] and stats["totals"]["failed"]:
        outcome = "fail"
    elif args.exit_code:
        outcome = "fail"

    run = {
        "type": "run",
        "shard": args.shard,
        "scenario": args.scenario,
        "seed": args.seed,
        "sha": args.sha,
        "compiler": args.compiler,
        "config": args.config,
        "exit_code": args.exit_code,
        "duration_s": args.duration_s,
        "outcome": outcome,
        "tests_seen": stats["tests_seen"],
        "declared_total": stats["declared_total"],
        "reached_summary": stats["reached_summary"],
        "crashes": stats["crashes"],
        "totals": stats["totals"],
        "log_bytes": len(text),
        "host_noise_stripped": stats["host_noise_stripped"],
    }

    stream = open(args.out, "w") if args.out else sys.stdout
    try:
        # record first so a truncated file still identifies itself
        for rec in [run] + records:
            rec.setdefault("v", SCHEMA_VERSION)
            stream.write(json.dumps(rec, sort_keys=True) + "\n")
    finally:
        if args.out:
            stream.close()

    print(
        "parsed %s: %d tests, %d crashes, outcome=%s"
        % (args.log, stats["tests_seen"], stats["crashes"], outcome),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
