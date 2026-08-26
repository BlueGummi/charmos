"""Merge per-run result records into markdown and JSON reports."""

import html
import json
import re
import sys
from collections import defaultdict
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path
from typing import Any

OUTCOME_ICON = {
    "pass": "✅",
    "fail": "❌",
    "crash": "💥",
    "timeout": "⏱️",
    "incomplete": "⚠️",
}

BAD_OUTCOMES = {"fail", "crash", "timeout", "incomplete"}
TOP_FRAMES_SHOWN = 6
RESULT_VERSION = 1

Record = dict[str, Any]


def load(
    paths: Iterable[Path],
) -> tuple[list[Record], list[Record], list[Record], list[Path]]:
    runs, tests, crashes, stale = [], [], [], set[Path]()
    for path in paths:
        with path.open(encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    print(
                        f"warning: {path}:{lineno} is not valid JSON; skipping",
                        file=sys.stderr,
                    )
                    continue
                if not isinstance(rec, dict):
                    print(
                        f"warning: {path}:{lineno} is not a JSON object; skipping",
                        file=sys.stderr,
                    )
                    continue
                rec["_source"] = path.name
                version = rec.get("v")
                if version != RESULT_VERSION and path not in stale:
                    print(
                        f"warning: {path} is v{version}, this reads v{RESULT_VERSION}",
                        file=sys.stderr,
                    )
                    stale.add(path)
                kind = rec.get("type")
                if kind == "run":
                    runs.append(rec)
                elif kind == "test":
                    tests.append(rec)
                elif kind == "crash":
                    crashes.append(rec)
    return runs, tests, crashes, sorted(stale)


def shard_of(rec: Record, runs_by_source: dict[str, Record]) -> Record:
    source = rec.get("_source")
    if not isinstance(source, str):
        return {}
    return runs_by_source.get(source) or {}


def md_escape(text: object) -> str:
    """Escape text embedded in a Markdown table cell."""
    return (
        str("" if text is None else text)
        .replace("\\", "\\\\")
        .replace("|", "\\|")
        .replace("\r", " ")
        .replace("\n", " ")
    )


def md_code(value: object) -> str:
    text = md_escape(value)
    delimiter = "`" * (
        max((len(match) for match in re.findall(r"`+", text)), default=0) + 1
    )
    return f"{delimiter}{text}{delimiter}"


def md_cell(value: object, *, code: bool = False) -> str:
    """Render a non-empty Markdown table cell that cannot end its row."""
    if value is None or value == "":
        return "-"
    return md_code(value) if code else md_escape(value)


def md_table(
    add: Callable[[str], None], headers: Sequence[str], rows: Iterable[Sequence[str]]
) -> None:
    add(f"| {' | '.join(headers)} |")
    add(f"| {' | '.join('---' for _ in headers)} |")
    for row in rows:
        if len(row) != len(headers):
            raise ValueError(
                f"row {row!r} has {len(row)} cells; the table has {len(headers)} columns"
            )
        add(f"| {' | '.join(row)} |")


def build_report(
    runs: list[Record], tests: list[Record], crashes: list[Record], repro_template: str
) -> Record:
    runs_by_source = {r["_source"]: r for r in runs}

    by_sig: defaultdict[str, dict[str, Any]] = defaultdict(
        lambda: {"count": 0, "occurrences": [], "example": None}
    )
    for c in crashes:
        entry = by_sig[c.get("signature", "unknown")]
        entry["count"] += 1
        entry["example"] = entry["example"] or c
        run = shard_of(c, runs_by_source)
        entry["occurrences"].append(
            {
                "shard": run.get("shard"),
                "scenario": run.get("scenario"),
                "seed": run.get("seed"),
                "compiler": run.get("compiler"),
                "config": run.get("config"),
            }
        )

    unique_crashes = sorted(by_sig.items(), key=lambda kv: kv[1]["count"], reverse=True)

    notable = [t for t in tests if t.get("status") in ("fail", "flaky")]

    warnings = []
    for r in runs:
        label = r.get("shard") or r["_source"]
        if not r.get("reached_summary"):
            warnings.append(f"shard {md_code(label)} never reached the summary line")
        seen, declared = r.get("tests_seen"), r.get("declared_total")
        if (
            r.get("reached_summary")
            and declared
            and seen is not None
            and seen != declared
        ):
            warnings.append(
                f"shard {md_code(label)} reported {seen} test results but the "
                f"harness declared {declared}"
            )
        for detail in r.get("schema_errors", []):
            warnings.append(f"shard {md_code(label)} schema drift: {detail}")

    failed_runs = [r for r in runs if r.get("outcome") in BAD_OUTCOMES]
    ok = not failed_runs and not unique_crashes

    return {
        "ok": ok,
        "runs": runs,
        "failed_runs": failed_runs,
        "unique_crashes": unique_crashes,
        "notable": notable,
        "warnings": warnings,
        "repro_template": repro_template,
    }


def render_markdown(rep: Record) -> str:
    runs = rep["runs"]
    out: list[str] = []
    add = out.append

    total = len(runs)
    bad = len(rep["failed_runs"])
    if rep["ok"]:
        add(f"## ✅ All {total} shard{'s' if total != 1 else ''} clean\n")
    else:
        add(
            f"## ❌ {bad}/{total} shard{'s' if total != 1 else ''} failed, "
            f"{len(rep['unique_crashes'])} unique crash"
            f"{'es' if len(rep['unique_crashes']) != 1 else ''}\n"
        )

    if rep["warnings"]:
        add("### ⚠️ Warnings\n")
        for w in rep["warnings"]:
            add(f"- {w}")
        add("")

    if rep["unique_crashes"]:
        add("### Unique crashes\n")
        for sig, entry in rep["unique_crashes"]:
            ex = entry["example"]
            hits = entry["count"]
            add(
                f"<details><summary><code>{html.escape(str(sig))}</code> &mdash; "
                f"{html.escape(str(ex.get('kind', '?')))}: "
                f"{html.escape(str(ex.get('message') or '(no message)'))} "
                f"({hits} occurrence{'s' if hits != 1 else ''})</summary>\n"
            )
            if ex.get("site"):
                add(f"**Site:** {md_code(ex['site'])}")
            if ex.get("function"):
                add(f"**Function:** {md_code(ex['function'])}")
            frames = ex.get("frames") or []
            if frames:
                add("\n```")
                for f in frames[:TOP_FRAMES_SHOWN]:
                    add(
                        f"#{f.get('index', 0):<2} {f.get('address', '?')}  "
                        f"{f.get('symbol', '?')}+0x{f.get('offset', 0):x}"
                    )
                if len(frames) > TOP_FRAMES_SHOWN:
                    add(f"... {len(frames) - TOP_FRAMES_SHOWN} more frames")
                add("```\n")

            add("**Seen in:**\n")
            md_table(
                add,
                ("shard", "scenario", "seed", "compiler"),
                [
                    (
                        md_cell(occ.get("shard")),
                        md_cell(occ.get("scenario")),
                        md_cell(occ.get("seed"), code=True),
                        md_cell(occ.get("compiler")),
                    )
                    for occ in entry["occurrences"]
                ],
            )
            first_seed = next(
                (o["seed"] for o in entry["occurrences"] if o.get("seed")), None
            )
            if first_seed and rep["repro_template"]:
                add("\n**Reproduce:**\n")
                add("```sh")
                add(
                    rep["repro_template"]
                    .replace("{seed}", str(first_seed))
                    .replace(
                        "{scenario}",
                        str(entry["occurrences"][0].get("scenario") or ""),
                    )
                )
                add("```")
            add("\n</details>\n")

    add("### Shards\n")
    rows: list[tuple[str, ...]] = []
    for r in sorted(runs, key=lambda run_rec: str(run_rec.get("shard") or "")):
        t = r.get("totals") or {}
        dur = r.get("duration_s")
        rows.append(
            (
                OUTCOME_ICON.get(r.get("outcome"), "❔"),
                md_cell(r.get("shard")),
                md_cell(r.get("scenario")),
                md_cell(r.get("outcome")),
                md_cell(r.get("tests_seen")),
                md_cell(t.get("passed")),
                md_cell(t.get("failed")),
                md_cell(t.get("skipped")),
                f"{dur:.0f}s" if dur is not None else "-",
            )
        )
    md_table(
        add,
        ("", "shard", "scenario", "outcome", "tests", "pass", "fail", "skip", "time"),
        rows,
    )
    add("")

    if rep["notable"]:
        add("### Failed + flaky tests\n")
        rows = []
        for t in rep["notable"]:
            runs_info = t.get("runs")
            detail = t.get("message") or ""
            if runs_info:
                detail = f"{runs_info.get('failed', 0)}/{runs_info.get('requested', 0)} runs failed"
            rows.append(
                (
                    md_cell(t.get("name"), code=True),
                    md_cell(t.get("group")),
                    md_cell(t.get("tier")),
                    md_cell(t.get("status")),
                    md_cell(detail),
                )
            )
        md_table(add, ("test", "group", "tier", "status", "detail"), rows)
        add("")

    return "\n".join(out) + "\n"


def render_json(rep: Record) -> str:
    report_json = {
        "ok": rep["ok"],
        "shards": len(rep["runs"]),
        "failed_shards": len(rep["failed_runs"]),
        "unique_crashes": [
            {
                "signature": sig,
                "count": e["count"],
                "kind": e["example"].get("kind"),
                "site": e["example"].get("site"),
                "message": e["example"].get("message"),
                "frames": e["example"].get("frames", []),
                "occurrences": e["occurrences"],
            }
            for sig, e in rep["unique_crashes"]
        ],
        "notable_tests": rep["notable"],
        "warnings": rep["warnings"],
    }
    return f"{json.dumps(report_json, indent=2, sort_keys=True)}\n"
