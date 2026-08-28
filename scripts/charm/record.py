"""Read the kernel's machine channel into result records."""

import hashlib
import json
import re
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from typing import Any, Final

from . import protocol as P

RESULT_FORMAT_VERSION: Final = 1

EXIT_PANIC: Final = 3
EXIT_TIMEOUT: Final = 124

SIGNATURE_FRAMES: Final = 5

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
    in_flight_test: Record | None = None
    recent_logs: list[Record] = field(default_factory=list)


@dataclass
class ParseResult:
    records: list[Record]
    stats: ParseStats
    supplied: set[RecordKey]


@dataclass(frozen=True)
class RunMeta:
    """Identity the host knows and the guest does not."""

    shard: str | None = None
    scenario: str | None = None
    seed: str | None = None
    sha: str | None = None
    compiler: str | None = None
    config: str | None = None
    exit_code: int | None = None
    duration_s: float | None = None


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
    out: Record = {
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

    out: Record = {
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


def ndjson_log_record(rec: Record) -> Record:
    return {
        "type": "log",
        "site": rec.get("site"),
        "level": rec.get("level"),
        "msg": rec.get("msg"),
        "file": rec.get("file"),
        "line": rec.get("line"),
        "func": rec.get("func"),
        "t_ms": rec.get(P.KEY_TIME),
        "cpu": rec.get(P.KEY_CPU),
    }


NDJSON_HANDLERS = {
    (P.DOMAIN_LOG, P.KIND_MESSAGE): handler(
        1,
        reads=("site", "level", "msg", "file", "line", "func"),
        fn=ndjson_log_record,
    ),
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
    """Parse an NDJSON line stream."""
    records: list[Record] = []
    schema: Schema = {}
    stats = ParseStats()
    supplied: set[RecordKey] = set()
    last_crash = None
    current_group: str | None = None
    in_flight_test: Record | None = None
    recent_logs: list[Record] = []

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

        if out["type"] == "group_start":
            current_group = out.get("group")
        elif out["type"] == "group_end":
            current_group = None
        elif out["type"] == "log":
            recent_logs.append(out)
            if len(recent_logs) > 50:
                recent_logs.pop(0)
            msg = out.get("msg") or ""
            m = re.match(
                r"^test start:\s*(?:([a-zA-Z0-9_]+):)?([a-zA-Z0-9_]+)\s*\(([a-zA-Z0-9_]+)\)",
                msg,
            )
            if m:
                grp = m.group(1) or current_group
                in_flight_test = {
                    "type": "test",
                    "group": grp,
                    "tier": m.group(3),
                    "name": m.group(2),
                    "status": "incomplete",
                    "duration_ms": 0,
                    "t_ms": out.get("t_ms"),
                }
        elif out["type"] == "test":
            in_flight_test = None

        if out["type"] == "totals":
            stats.reached_summary = True
            stats.totals = {k: out[k] for k in ("total", "passed", "failed", "skipped")}
        elif out["type"] == "verdict":
            stats.verdict = {"ok": out["ok"], "duration_ms": out["duration_ms"]}

    stats.recent_logs = recent_logs
    if in_flight_test and (not stats.ended or not stats.reached_summary):
        stats.in_flight_test = in_flight_test
        in_flight_synth = dict(in_flight_test)
        in_flight_synth["status"] = classify(stats, stats.exit_code)
        in_flight_synth["message"] = (
            f"Halted/timed out during test '{in_flight_test['name']}'"
        )
        records.append(in_flight_synth)

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


def classify(stats: ParseStats, exit_code: int | None) -> str:
    """Classify the outcome, most severe first."""
    if exit_code == EXIT_PANIC or stats.crashes:
        return "crash"
    if exit_code == EXIT_TIMEOUT:
        return "timeout"
    if not stats.ended or not stats.reached_summary:
        return "incomplete"
    if stats.totals and stats.totals["failed"]:
        return "fail"
    if exit_code:
        return "fail"
    return "pass"


def build_run_record(result: ParseResult, meta: RunMeta, log_bytes: int) -> Record:
    """Build run record from parsed results and host metadata."""
    stats, supplied = result.stats, result.supplied

    run: Record = {
        "type": "run",
        "shard": meta.shard,
        "scenario": meta.scenario,
        "seed": meta.seed,
        "sha": meta.sha,
        "compiler": meta.compiler,
        "config": meta.config,
        "exit_code": meta.exit_code,
        "duration_s": meta.duration_s,
        "outcome": classify(stats, meta.exit_code),
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

    if stats.in_flight_test:
        run["in_flight_test"] = stats.in_flight_test
    if stats.recent_logs:
        run["recent_logs"] = stats.recent_logs

    if stats.schema_errors:
        run["schema_errors"] = stats.schema_errors

    if stats.ended:
        run["guest_exit_code"] = stats.exit_code
        run["end_reason"] = stats.end_reason
    else:
        run["last_record"] = stats.last_record

    return run


def render(records: Iterable[Record]) -> str:
    """Stamp the format version and serialize, one record per line."""
    out = []
    for rec in records:
        rec.setdefault("v", RESULT_FORMAT_VERSION)
        out.append(f"{json.dumps(rec, sort_keys=True)}\n")
    return "".join(out)
