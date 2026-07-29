#!/usr/bin/env python3
"""Merge NDJSON results into one report

Writes GitHub-flavoured markdown to --summary 
and a merged JSON document to --json

Usage:
    aggregate.py results/*.ndjson --summary "$GITHUB_STEP_SUMMARY"
"""

import argparse
import glob
import json
import os
import sys
from collections import defaultdict

OUTCOME_ICON = {
    "pass": "✅",
    "fail": "❌",
    "crash": "💥",
    "timeout": "⏱️",
    "incomplete": "⚠️",
}

# "this shard did not produce a trustworthy clean result"
BAD_OUTCOMES = {"fail", "crash", "timeout", "incomplete"}

TOP_FRAMES_SHOWN = 6


def load(paths):
    runs, tests, crashes = [], [], []
    for path in paths:
        with open(path) as fh:
            for lineno, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    print(
                        "warning: %s:%d is not valid JSON, skipping" % (path, lineno),
                        file=sys.stderr,
                    )
                    continue
                rec["_source"] = os.path.basename(path)
                kind = rec.get("type")
                if kind == "run":
                    runs.append(rec)
                elif kind == "test":
                    tests.append(rec)
                elif kind == "crash":
                    crashes.append(rec)
    return runs, tests, crashes


def shard_of(rec, runs_by_source):
    run = runs_by_source.get(rec.get("_source"))
    if not run:
        return {}
    return run


def md_escape(text):
    return (text or "").replace("|", "\\|").replace("\n", " ")


def build_report(runs, tests, crashes, repro_template):
    runs_by_source = {r["_source"]: r for r in runs}

    by_sig = defaultdict(lambda: {"count": 0, "occurrences": [], "example": None})
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
            warnings.append(
                "shard `%s` never reached the harness summary line "
                "(died early, or the log format moved and the parser "
                "stopped matching)" % label
            )
        seen, declared = r.get("tests_seen"), r.get("declared_total")
        if declared and seen is not None and seen != declared:
            warnings.append(
                "shard `%s` parsed %d test results but the harness declared "
                "%d - the parser is likely out of date with the output format"
                % (label, seen, declared)
            )

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


def render_markdown(rep):
    runs = rep["runs"]
    out = []
    add = out.append

    total = len(runs)
    bad = len(rep["failed_runs"])
    if rep["ok"]:
        add("## ✅ All %d shard%s clean\n" % (total, "" if total == 1 else "s"))
    else:
        add(
            "## ❌ %d/%d shard%s failed, %d unique crash%s\n"
            % (
                bad,
                total,
                "" if total == 1 else "s",
                len(rep["unique_crashes"]),
                "" if len(rep["unique_crashes"]) == 1 else "es",
            )
        )

    if rep["warnings"]:
        add("### ⚠️ Warnings\n")
        for w in rep["warnings"]:
            add("- %s" % w)
        add("")

    if rep["unique_crashes"]:
        add("### Unique crashes\n")
        for sig, entry in rep["unique_crashes"]:
            ex = entry["example"]
            hits = entry["count"]
            add(
                "<details><summary><code>%s</code> &mdash; %s: %s "
                "(%d occurrence%s)</summary>\n"
                % (
                    sig,
                    ex.get("kind", "?"),
                    md_escape(ex.get("message") or "(no message)"),
                    hits,
                    "" if hits == 1 else "s",
                )
            )
            if ex.get("site"):
                add("**Site:** `%s`" % ex["site"])
            if ex.get("function"):
                add("**Function:** `%s`" % ex["function"])
            frames = ex.get("frames") or []
            if frames:
                add("\n```")
                for f in frames[:TOP_FRAMES_SHOWN]:
                    add(
                        "#%-2d %s  %s+0x%x"
                        % (
                            f.get("index", 0),
                            f.get("address", "?"),
                            f.get("symbol", "?"),
                            f.get("offset", 0),
                        )
                    )
                if len(frames) > TOP_FRAMES_SHOWN:
                    add("... %d more frames" % (len(frames) - TOP_FRAMES_SHOWN))
                add("```\n")

            add("**Seen in:**\n")
            add("| shard | scenario | seed | compiler |")
            add("| --- | --- | --- | --- |")
            for occ in entry["occurrences"]:
                add(
                    "| %s | %s | `%s` | %s |"
                    % (
                        occ.get("shard") or "-",
                        occ.get("scenario") or "-",
                        occ.get("seed") or "-",
                        occ.get("compiler") or "-",
                    )
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
    add("| | shard | scenario | outcome | tests | pass | fail | skip | time |")
    add("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in sorted(runs, key=lambda r: str(r.get("shard"))):
        t = r.get("totals") or {}
        dur = r.get("duration_s")
        add(
            "| %s | %s | %s | %s | %s | %s | %s | %s | %s |"
            % (
                OUTCOME_ICON.get(r.get("outcome"), "❔"),
                r.get("shard") or "-",
                r.get("scenario") or "-",
                r.get("outcome") or "-",
                r.get("tests_seen", "-"),
                t.get("passed", "-"),
                t.get("failed", "-"),
                t.get("skipped", "-"),
                "%.0fs" % dur if dur else "-",
            )
        )
    add("")

    if rep["notable"]:
        add("### Failed and flaky tests\n")
        add("| test | group | tier | status | detail |")
        add("| --- | --- | --- | --- | --- |")
        for t in rep["notable"]:
            runs_info = t.get("runs")
            detail = md_escape(t.get("message") or "")
            if runs_info:
                detail = "%d/%d runs failed" % (
                    runs_info.get("failed", 0),
                    runs_info.get("requested", 0),
                )
            add(
                "| `%s` | %s | %s | %s | %s |"
                % (
                    t.get("name"),
                    t.get("group"),
                    t.get("tier"),
                    t.get("status"),
                    detail or "-",
                )
            )
        add("")

    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inputs", nargs="+", help="NDJSON files or globs")
    ap.add_argument("--summary", help="write markdown here (append if exists)")
    ap.add_argument("--json", help="write merged JSON report here")
    ap.add_argument(
        "--repro-template",
        default="./scripts/build.sh -t RelWithDebInfo tests -- "
        '-DKERNEL_CMDLINE="test.seed={seed}"',
        help="shell snippet shown for reproducing a crash; {seed} and "
        "{scenario} are substituted",
    )
    ap.add_argument(
        "--fail-on-error",
        action="store_true",
        help="exit 1 if any shard failed (gate jobs want this; hunt jobs "
        "generally do not, since a finding is an issue and not a red X)",
    )
    args = ap.parse_args()

    paths = []
    for pattern in args.inputs:
        expanded = sorted(glob.glob(pattern))
        paths.extend(expanded if expanded else [pattern])
    paths = [p for p in paths if os.path.isfile(p)]

    if not paths:
        print("error: no result files matched", file=sys.stderr)
        return 2

    runs, tests, crashes = load(paths)
    if not runs:
        print("error: no run records found in %d file(s)" % len(paths), file=sys.stderr)
        return 2

    rep = build_report(runs, tests, crashes, args.repro_template)
    markdown = render_markdown(rep)

    if args.summary:
        with open(args.summary, "a") as fh:
            fh.write(markdown)
    else:
        sys.stdout.write(markdown)

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(
                {
                    "ok": rep["ok"],
                    "shards": len(runs),
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
                },
                fh,
                indent=2,
                sort_keys=True,
            )

    print(
        "%d shard(s), %d failed, %d unique crash(es)"
        % (len(runs), len(rep["failed_runs"]), len(rep["unique_crashes"])),
        file=sys.stderr,
    )

    return 1 if (args.fail_on_error and not rep["ok"]) else 0


if __name__ == "__main__":
    sys.exit(main())
