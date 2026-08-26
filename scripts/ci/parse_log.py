#!/usr/bin/env python3
"""Turn a QEMU run into NDJSON result records


Why NDJSON? So truncated lines only affect that line 

The console log is read, but only for size so empty logs mean
QEMU didn't even reach the guest

Usage:
    parse_log.py output.log --ndjson ndjson.log --shard 3 --out results.ndjson
"""

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Final, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ndjson_protocol as P

RESULT_FORMAT_VERSION: Final = 1

# Host statuses from ``run_qemu.sh``
EXIT_PANIC = 3
EXIT_TIMEOUT = 124  # by convention, from timeout(1)

SIGNATURE_FRAMES = 5

# Ignore addresses and counters
HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
NUM_RE = re.compile(r"\b\d+\b")


Record = dict[str, Any]
RecordKey = tuple[str, str]
Schema = dict[RecordKey, dict[str, Any]]
SCHEMA_RECORD_FIELDS = frozenset({"domain", "kind", "rec_version", "field", "type"})


@dataclass
class ParseStats:

    declared_total: int | None = None
    totals: Record | None = None
    verdict: Record | None = None
    reached_summary: bool = False
    malformed_lines: int = 0
    records_seen: int = 0
    truncated_records: int = 0
    exit_code: int | None = None
    schema_errors: list[str] = field(default_factory=list)
    schema_seen: bool = False
    ended: bool = False
    end_reason: str | None = None
    last_record: str | None = None
    tests_seen: int = 0
    crashes: int = 0


@dataclass
class ParseResult:

    records: list[Record]
    stats: ParseStats
    supplied: set[RecordKey]


def normalize_for_signature(text: str) -> str:
    text = HEX_RE.sub("<addr>", text)
    return NUM_RE.sub("<n>", text)


def crash_signature(
    kind: str, site: str | None, message: str | None, frames: list[Record]
) -> str:
    parts = [kind, site or "", normalize_for_signature(message or "")]
    parts.extend(f.get("symbol", "?") for f in frames[:SIGNATURE_FRAMES])
    digest = hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()
    return digest[:12]


# ``include/ndjson.h`` via ``ndjson_protocol`` gives us the literals,
# so we can handle discrepancies

@dataclass(frozen=True)
class Handler:
    version: int
    reads: tuple[str, ...]
    fn: Callable[[Record], Record]
    attaches: str | None = None


def handler(
    version: int,
    fn: Callable[[Record], Record],
    reads: Iterable[str] = (),
    attaches: str | None = None,
) -> Handler:
    return Handler(version, tuple(reads), fn, attaches)


def ndjson_payload(rec: Record) -> Record:
    return {k: v for k, v in rec.items() if k not in P.ENVELOPE_KEYS}


def ndjson_frame(rec: Record) -> Record:
    return {
        "index": rec.get("idx", 0),
        "address": rec.get("addr"),
        "symbol": rec.get("sym") or "?",
        "offset": rec.get("off", 0),
    }


def ndjson_test_record(rec: Record) -> Record:
    out = {
        "type": "test",
        "group": rec.get("group"),
        "tier": rec.get("tier"),
        "name": rec.get("name"),
        "status": rec.get("status"),
        "duration_ms": rec.get("duration_ms"),
        "t_ms": rec.get(P.KEY_TIME),
    }
    if rec.get("reason"):
        out["reason"] = rec["reason"]
    if rec.get("msg"):
        out["message"] = rec["msg"]
    if rec.get("runs_requested"):
        out["runs"] = {
            "attempted": rec.get("runs_attempted", 0),
            "requested": rec["runs_requested"],
            "failed": rec.get("runs_failed", 0),
            "skipped": rec.get("runs_skipped", 0),
        }
    return out


def ndjson_crash_record(rec: Record) -> Record:
    kind = "panic" if rec[P.KEY_DOMAIN] == P.DOMAIN_PANIC else "asan"
    site = None
    if rec.get("file") is not None and rec.get("line") is not None:
        site = f"{rec['file']}:{rec['line']}"

    out = {
        "type": "crash",
        "kind": kind,
        "site": site,
        "function": rec.get("func"),
        "message": rec.get("msg") or rec.get("what"),
        "frames": [],
    }
    if rec.get("addr"):
        out["address"] = rec["addr"]
    return out


NDJSON_HANDLERS = {
    (P.DOMAIN_TEST, P.KIND_GROUP_START): handler(
        1,
        reads=("group", "test_count", "file"),
        fn=lambda r: {
            "type": "group_start",
            "group": r.get("group"),
            "test_count": r.get("test_count"),
            "file": r.get("file"),
        },
    ),
    (P.DOMAIN_TEST, P.KIND_RESULT): handler(
        1,
        reads=("group", "tier", "name", "status", "duration_ms"),
        fn=ndjson_test_record,
    ),
    (P.DOMAIN_TEST, P.KIND_GROUP_END): handler(
        1,
        reads=("group", "duration_ms", "failed", "skipped"),
        fn=lambda r: {
            "type": "group_end",
            "group": r.get("group"),
            "duration_ms": r.get("duration_ms"),
            "failed": r.get("failed", 0),
            "skipped": r.get("skipped", 0),
        },
    ),
    (P.DOMAIN_TEST, P.KIND_TOTALS): handler(
        1,
        reads=("total", "passed", "failed", "skipped"),
        fn=lambda r: {
            "type": "totals",
            "total": r.get("total"),
            "passed": r.get("passed"),
            "failed": r.get("failed"),
            "skipped": r.get("skipped"),
        },
    ),
    (P.DOMAIN_TEST, P.KIND_VERDICT): handler(
        1,
        reads=("ok", "duration_ms"),
        fn=lambda r: {
            "type": "verdict",
            "ok": r.get("ok"),
            "duration_ms": r.get("duration_ms"),
        },
    ),
    (P.DOMAIN_PANIC, P.KIND_AT): handler(
        1,
        reads=("file", "line", "func", "msg"),
        fn=ndjson_crash_record,
    ),
    (P.DOMAIN_ASAN, P.KIND_FAULT): handler(
        1,
        reads=("what", "addr", "size", "access"),
        fn=ndjson_crash_record,
    ),
    (P.DOMAIN_PANIC, P.KIND_FRAME): handler(
        1,
        reads=("idx", "addr", "sym", "off"),
        fn=ndjson_frame,
        attaches="frames",
    ),
    (P.DOMAIN_ASAN, P.KIND_FRAME): handler(
        1,
        reads=("idx", "addr", "sym", "off"),
        fn=ndjson_frame,
        attaches="frames",
    ),
}


def ndjson_schema_check(
    schema: Schema, handlers: dict[RecordKey, Handler] | None = None
) -> list[str]:
    handlers = NDJSON_HANDLERS if handlers is None else handlers
    errors = []

    for (domain, kind), h in sorted(handlers.items()):
        entry = schema.get((domain, kind))
        if entry is None:
            continue

        if entry["version"] != h.version:
            errors.append(
                f"{domain}/{kind}: this parser handles v{h.version}, the kernel "
                f"declares v{entry['version']}"
            )
            continue

        declared = {f["name"] for f in entry["fields"]}
        missing = [name for name in h.reads if name not in declared]
        if missing:
            errors.append(
                f"{domain}/{kind}: parser reads {', '.join(missing)}, which the "
                f"kernel does not declare (it declares {', '.join(sorted(declared)) or 'nothing'})"
            )

    return errors


def parse_ndjson(lines: Iterable[str]) -> ParseResult:
    """Parse an NDJSON line """
    records: list[Record] = []
    schema: Schema = {}
    stats = ParseStats()
    supplied: set[RecordKey] = set()
    last_crash = None

    for line in lines:
        line = line.strip()
        if not line:
            continue

        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            stats.malformed_lines += 1
            continue

        if (
            not isinstance(rec, dict)
            or P.KEY_DOMAIN not in rec
            or P.KEY_KIND not in rec
        ):
            stats.malformed_lines += 1
            continue

        stats.records_seen += 1
        stats.last_record = f"{rec[P.KEY_DOMAIN]}/{rec[P.KEY_KIND]}"
        if rec.get(P.KEY_TRUNCATED):
            stats.truncated_records += 1

        key = (rec[P.KEY_DOMAIN], rec[P.KEY_KIND])

        if key == (P.DOMAIN_NDJSON, P.KIND_BYE):
            stats.ended = True
            stats.exit_code = rec.get("code")
            stats.end_reason = rec.get("reason")
            supplied.add(key)
            continue

        if key == (P.DOMAIN_NDJSON, P.KIND_SCHEMA):
            if not SCHEMA_RECORD_FIELDS.issubset(rec):
                stats.malformed_lines += 1
                continue
            stats.schema_seen = True
            entry = schema.setdefault(
                (rec["domain"], rec["kind"]),
                {"version": rec.get("rec_version"), "fields": []},
            )
            entry["fields"].append({"name": rec.get("field"), "type": rec.get("type")})
            continue

        if key == (P.DOMAIN_TEST, P.KIND_BEGIN):
            stats.declared_total = rec.get("declared_total")
            supplied.add(key)
            continue

        h = NDJSON_HANDLERS.get(key)
        if h is None:
            # Preserve declared but not-yet-normalized records in the result stream.
            records.append(
                {
                    "type": "ndjson",
                    "domain": rec[P.KEY_DOMAIN],
                    "kind": rec[P.KEY_KIND],
                    "record": ndjson_payload(rec),
                }
            )
            last_crash = None
            continue

        supplied.add(key)

        if h.attaches:
            if last_crash is not None:
                last_crash.setdefault(h.attaches, []).append(h.fn(rec))
            continue

        out = h.fn(rec)
        records.append(out)

        last_crash = out if out["type"] == "crash" else None

        if out["type"] == "totals":
            stats.reached_summary = True
            stats.totals = {k: out[k] for k in ("total", "passed", "failed", "skipped")}
        elif out["type"] == "verdict":
            stats.verdict = {"ok": out["ok"], "duration_ms": out["duration_ms"]}

    for crash in records:
        if crash.get("type") == "crash":
            crash["signature"] = crash_signature(
                crash["kind"],
                crash.get("site"),
                crash.get("message"),
                crash.get("frames", []),
            )

    stats.schema_errors = ndjson_schema_check(schema)

    if schema:
        records.append(
            {
                "type": "schema",
                "records": {
                    f"{domain}/{kind}": entry
                    for (domain, kind), entry in schema.items()
                },
            }
        )

    stats.tests_seen = sum(1 for r in records if r["type"] == "test")
    stats.crashes = sum(1 for r in records if r["type"] == "crash")
    return ParseResult(records, stats, supplied)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="QEMU serial log, kept for its size only")
    ap.add_argument(
        "--ndjson",
        required=True,
        help="the kernel's machine channel NDJSON",
    )
    ap.add_argument(
        "--strict-schema",
        action="store_true",
        help="exit non-zero when a handler disagrees with the emitted schema",
    )
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
        "3 panic, 124 timeout by convention)",
    )
    args = ap.parse_args()

    try:
        log_bytes = Path(args.log).stat().st_size
    except FileNotFoundError:
        log_bytes = 0

    try:
        with Path(args.ndjson).open(encoding="utf-8", errors="replace") as fh:
            result = parse_ndjson(fh)
    except FileNotFoundError:
        print(
            f"error: {args.ndjson} not found",
            file=sys.stderr,
        )
        return 2

    records, stats, supplied = result.records, result.stats, result.supplied

    for err in stats.schema_errors:
        print(f"schema drift: {err}", file=sys.stderr)
        records.append({"type": "schema_error", "detail": err})

    if not stats.schema_seen:
        print(
            f"warning: {args.ndjson} carries no schema records, so handlers went "
            "unchecked. Boot with ndjson.schema=true",
            file=sys.stderr,
        )

    outcome = "pass"
    if args.exit_code == EXIT_PANIC:
        outcome = "crash"
    elif stats.crashes:
        outcome = "crash"
    elif args.exit_code == EXIT_TIMEOUT:
        outcome = "timeout"
    elif not stats.ended:
        outcome = "incomplete"
    elif not stats.reached_summary:
        outcome = "incomplete"
    elif stats.totals and stats.totals["failed"]:
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
        "tests_seen": stats.tests_seen,
        "declared_total": stats.declared_total,
        "reached_summary": stats.reached_summary,
        "crashes": stats.crashes,
        "totals": stats.totals,
        "log_bytes": log_bytes,
        "machine_channel": {
            "records_seen": stats.records_seen,
            "malformed_lines": stats.malformed_lines,
            "truncated_records": stats.truncated_records,
            "supplied": sorted(f"{domain}/{kind}" for domain, kind in supplied),
        },
        "schema_ok": (not stats.schema_errors if stats.schema_seen else None),
        "guest_ended": stats.ended,
    }

    if stats.schema_errors:
        run["schema_errors"] = stats.schema_errors

    if stats.ended:
        run["guest_exit_code"] = stats.exit_code
        run["end_reason"] = stats.end_reason
    else:
        run["last_record"] = stats.last_record

    output = []
    for rec in [run, *records]:
        rec.setdefault("v", RESULT_FORMAT_VERSION)
        output.append(f"{json.dumps(rec, sort_keys=True)}\n")
    if args.out:
        Path(args.out).write_text("".join(output), encoding="utf-8")
    else:
        sys.stdout.write("".join(output))

    if not stats.ended:
        last_record = (
            f", last record was {stats.last_record}" if stats.last_record else ""
        )
        print(
            f"guest never said goodbye: it stopped rather than finished{last_record}",
            file=sys.stderr,
        )

    print(
        f"parsed {args.ndjson}: {stats.records_seen} records, "
        f"{stats.tests_seen} tests, {stats.crashes} crashes, outcome={outcome}",
        file=sys.stderr,
    )

    if stats.schema_errors:
        print(
            f"{len(stats.schema_errors)} handler(s) disagree with the kernel's declared schema",
            file=sys.stderr,
        )
        if args.strict_schema:
            return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
