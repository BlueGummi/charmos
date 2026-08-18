#!/usr/bin/env python3
"""report tags (TODO, FIX, HACK, ...)

--staged   gives the current commit's delta, used by pre-commit hook
--full     full review of all tags everywhere

"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter, defaultdict

KINDS = {
    "TODO": [],
    "FIX": ["FIXME", "FIXIT", "ISSUE"],
    "BUG": [],
    "HACK": [],
    "WARN": ["WARNING", "XXX"],
    "PERF": ["OPTIM", "OPTIMIZE", "PERFORMANCE"],
    "NOTE": ["INFO"],
    "TEST": ["TESTING"],
}

OUTWARD = {"WARN", "BUG"}

ALIAS = {alt: kind for kind, alts in KINDS.items() for alt in alts}
ALIAS.update({k: k for k in KINDS})

# TAG(subclass):
TAG_RE = re.compile(
    r"\b("
    + "|".join(sorted(ALIAS, key=len, reverse=True))
    + r")\b[ \t]*(?:\(([^)]*)\))?[ \t]*:"
)

VENDORED = (
    "kernel/uACPI/",
    "kernel/flanterm/",
    "limine/",
    "build/",
    "include/limine.h",
)
SOURCE_EXT = (
    ".c",
    ".h",
    ".cc",
    ".cpp",
    ".hpp",
    ".S",
    ".asm",
    ".py",
    ".sh",
    ".cmake",
    ".md",
    ".ld",
    ".awk",
)

C = {
    "red": "\033[31m",
    "grn": "\033[32m",
    "yel": "\033[33m",
    "blu": "\033[34m",
    "mag": "\033[35m",
    "cya": "\033[36m",
    "dim": "\033[2m",
    "bld": "\033[1m",
    "off": "\033[0m",
}
KIND_COLOR = {
    "TODO": "blu",
    "FIX": "red",
    "BUG": "red",
    "HACK": "yel",
    "WARN": "yel",
    "PERF": "cya",
    "NOTE": "dim",
    "TEST": "mag",
}


def paint(s, color, enabled):
    return f"{C[color]}{s}{C['off']}" if enabled and color in C else s


def is_vendored(path):
    return path.startswith(VENDORED)


def interesting(path):
    return path.endswith(SOURCE_EXT) and not is_vendored(path)


def git(*args):
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, errors="replace"
    ).stdout


def find_tag(text):
    """return (kind, subclass, body) for the first tag on a line | None"""
    m = TAG_RE.search(text)
    if not m:
        return None
    body = text[m.end() :].strip()
    # Strip trailing comment punctuation so /* TODO: x */ is ok
    body = re.sub(r"\s*\*/\s*$", "", body).strip()
    return ALIAS[m.group(1)], m.group(2), body


def scan_diff(rev=None):
    args = ["diff", "-U0", "--no-color"]
    args += [rev] if rev else ["--cached"]
    added, removed, path = [], [], None
    new_ln = old_ln = 0

    for line in git(*args).split("\n"):
        if line.startswith("+++ b/"):
            path = line[6:]
            continue
        if line.startswith("@@"):
            m = re.match(r"@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
            if m:
                old_ln, new_ln = int(m.group(1)), int(m.group(2))
            continue
        if not path or not interesting(path):
            continue
        if line.startswith("+") and not line.startswith("+++"):
            hit = find_tag(line[1:])
            if hit:
                added.append((path, new_ln, *hit))
            new_ln += 1
        elif line.startswith("-") and not line.startswith("---"):
            hit = find_tag(line[1:])
            if hit:
                removed.append((path, old_ln, *hit))
            old_ln += 1
    return added, removed


def scan_tree():
    files = [f for f in git("ls-files").split("\n") if f and interesting(f)]
    rows = []
    for f in files:
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                if not TAG_RE.search(fh.read()):
                    continue
        except OSError:
            continue
        author_time = None
        for line in git("blame", "-w", "--line-porcelain", f).split("\n"):
            if line.startswith("author-time "):
                author_time = int(line.split()[1])
            elif line.startswith("\t") and author_time:
                hit = find_tag(line[1:])
                if hit:
                    rows.append((author_time, f, *hit))
    return rows


BUCKETS = [
    (7, "< 1 week"),
    (30, "< 1 month"),
    (90, "1-3 months"),
    (180, "3-6 months"),
    (10**6, "6+ months"),
]


def age_bucket(days):
    for limit, name in BUCKETS:
        if days < limit:
            return name
    return BUCKETS[-1][1]


def report_staged(added, removed, color):
    if not added and not removed:
        return 0

    if removed:
        print(paint(f"Resolving {len(removed)} tag(s):", "grn", color))
        for kind, n in Counter(r[2] for r in removed).most_common():
            print(f"  {paint(kind, 'grn', color)} x{n}")

    if not added:
        print()
        return 0

    print(paint(f"You are adding {len(added)} tag(s):", "bld", color))
    by_kind = defaultdict(lambda: defaultdict(list))
    for path, ln, kind, sub, _ in added:
        by_kind[kind][path].append((ln, sub))

    for kind in sorted(by_kind, key=lambda k: (k not in OUTWARD, k)):
        files = by_kind[kind]
        total = sum(len(v) for v in files.values())
        label = paint(f"{kind}:", KIND_COLOR.get(kind, "blu"), color)
        flag = paint("  (outward-facing)", "dim", color) if kind in OUTWARD else ""
        print(f"  {label} {total}{flag}")
        for path in sorted(files):
            entries = sorted(files[path])
            lines = ",".join(str(ln) for ln, _ in entries)
            subs = sorted({s for _, s in entries if s})
            tail = paint(f"  ({', '.join(subs)})", "dim", color) if subs else ""
            print(f"      {path}:{lines}{tail}")
    print()
    return 10


def report_full(rows, color):
    now = int(time.time())
    print(
        paint(
            f"{len(rows)} tags across " f"{len({r[1] for r in rows})} files\n",
            "bld",
            color,
        )
    )

    counts = Counter(age_bucket((now - r[0]) // 86400) for r in rows)
    print(paint("AGE", "bld", color))
    for _, name in BUCKETS:
        n = counts[name]
        bar = "#" * min(int(n / 2), 40)
        col = "yel" if name == "6+ months" and n else "dim"
        print(f"  {name:<12} {n:4d}  {paint(bar, col, color)}")

    print("\n" + paint("KIND", "bld", color))
    for kind, n in Counter(r[2] for r in rows).most_common():
        print(f"  {paint(kind, KIND_COLOR.get(kind, 'blu'), color):<16} {n:4d}")

    old = [r for r in rows if (now - r[0]) // 86400 >= 180]
    if old:
        print("\n" + paint("AGED CLUSTERS (6+ months, by directory)", "yel", color))
        by_dir = defaultdict(list)
        for t, f, *_ in old:
            by_dir[os.path.dirname(f)].append(t)
        for d, times in sorted(by_dir.items(), key=lambda kv: -len(kv[1])):
            if len(times) < 2:
                continue
            span = (now - max(times)) // 86400, (now - min(times)) // 86400
            print(f"  {len(times):3d} tags  {d + '/':<34} {span[0]}-{span[1]}d old")

    print("\n" + paint("UNTAGGED MASS (large, tag-free, tracked source)", "yel", color))
    tagged = {r[1] for r in rows}
    blobs = []
    for f in git("ls-files").split("\n"):
        if (
            not f
            or not interesting(f)
            or f in tagged
            or not f.endswith((".c", ".h"))
            or f.startswith("kernel/tests/")
        ):
            continue
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                n = sum(1 for _ in fh)
        except OSError:
            continue
        if n >= 400:
            blobs.append((n, f))
    for n, f in sorted(blobs, reverse=True)[:12]:
        print(f"  {n:5d} lines  {f}")
    if not blobs:
        print(paint("  (none)", "dim", color))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument(
        "--staged",
        action="store_true",
        help="tags added/removed by the staged diff (default)",
    )
    mode.add_argument(
        "--full",
        action="store_true",
        help="whole-tree census with ages and blob watchlist",
    )
    ap.add_argument(
        "--since", metavar="REV", help="diff against REV instead of the index"
    )
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()

    color = (
        not args.no_color and sys.stdout.isatty() and os.environ.get("TERM") != "dumb"
    )

    if args.full:
        rows = scan_tree()
        if args.json:
            now = int(time.time())
            json.dump(
                [
                    {
                        "time": t,
                        "age_days": (now - t) // 86400,
                        "file": f,
                        "kind": k,
                        "subclass": s,
                        "body": b,
                    }
                    for t, f, k, s, b in rows
                ],
                sys.stdout,
                indent=2,
            )
            print()
        else:
            report_full(rows, color)
        return 0

    added, removed = scan_diff(args.since)
    if args.json:
        json.dump(
            {
                "added": [
                    {"file": f, "line": l, "kind": k, "subclass": s, "body": b}
                    for f, l, k, s, b in added
                ],
                "removed": [
                    {"file": f, "line": l, "kind": k, "subclass": s, "body": b}
                    for f, l, k, s, b in removed
                ],
            },
            sys.stdout,
            indent=2,
        )
        print()
        return 0
    return report_staged(added, removed, color)


if __name__ == "__main__":
    sys.exit(main())
