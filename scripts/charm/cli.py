"""``charm`` the entry point for charmOS host tooling

Subcommand groups mirror the stage they serve:

    charm ci ...
    charm boot ...
    charm machine ...
    charm nightmare ...
"""

import glob
import json
import os
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Annotated, Any

from cyclopts import App, Parameter

from . import boot as B
from . import machine as MC
from . import protocol as P
from . import record as R
from . import report as RP
from . import schema as S
from . import workflow_policy as WP

app = App(name="charm", help=__doc__, result_action="return_value")
ci = App(name="ci", help="CI result parsing and reporting")
app.command(ci)


def fail(message: str, code: int = 2) -> int:
    print(f"error: {message}", file=sys.stderr)
    return code


@Parameter(name="*")
@dataclass
class ReportOptions:
    summary: Path | None = None
    """Write markdown here (append if exists)."""
    json: Path | None = None
    """Write merged JSON report here."""
    repro_template: str = ""
    """Shell snippet shown for reproducing a crash, with {seed} and {scenario}."""
    fail_on_error: bool = False
    """Exit 1 if any shard failed (gate jobs want this; hunt jobs generally do
    not, since a finding is an issue and not a red X)."""

    def write(self, rep: dict[str, Any], run_count: int) -> int:
        markdown = RP.render_markdown(rep)
        if self.summary:
            with self.summary.open("a", encoding="utf-8") as fh:
                fh.write(markdown)
        sys.stdout.write(markdown)
        if self.json:
            self.json.write_text(RP.render_json(rep), encoding="utf-8")
        print(
            f"{run_count} shard(s), {len(rep['failed_runs'])} failed, "
            f"{len(rep['unique_crashes'])} unique crash(es)",
            file=sys.stderr,
        )
        return 1 if (self.fail_on_error and not rep["ok"]) else 0


def _report(paths: list[Path], opts: ReportOptions) -> int:
    runs, tests, crashes, stale = RP.load(paths)
    if not runs:
        return fail(f"no run records found in {len(paths)} file(s)")
    rep = RP.build_report(runs, tests, crashes, opts.repro_template)
    rep["warnings"][:0] = [
        f"{RP.md_code(path)} was written by a different result version"
        for path in stale
    ]
    return opts.write(rep, len(runs))


@ci.command
def parse(
    log: Path,
    *,
    ndjson: Path,
    strict_schema: bool = False,
    out: Path | None = None,
    shard: str | None = None,
    scenario: str | None = None,
    seed: str | None = None,
    sha: str | None = None,
    compiler: str | None = None,
    config: str | None = None,
    duration_s: float | None = None,
    exit_code: int | None = None,
) -> int:
    """One run's logs -> result records.

    Parameters
    ----------
    log
        QEMU serial log, kept for its size only.
    ndjson
        The kernel's machine channel.
    strict_schema
        Exit non-zero when a handler disagrees with the emitted schema.
    out
        NDJSON output (default: stdout).
    config
        Free-form guest config label.
    exit_code
        Exit status of the QEMU wrapper (0 pass, 1 test failure, 3 panic,
        124 timeout by convention).
    """
    log_bytes = log.stat().st_size if log.is_file() else 0
    try:
        with ndjson.open(encoding="utf-8", errors="replace") as fh:
            result = R.parse_ndjson(fh)
    except FileNotFoundError:
        return fail(f"{ndjson} not found")

    stats = result.stats
    for err in stats.schema_errors:
        print(f"schema drift: {err}", file=sys.stderr)
        result.records.append({"type": "schema_error", "detail": err})
    if not stats.schema_seen:
        print(
            f"warning: {ndjson} contains no schema records (boot with ndjson.schema=true)",
            file=sys.stderr,
        )

    meta = R.RunMeta(
        shard=shard,
        scenario=scenario,
        seed=seed,
        sha=sha,
        compiler=compiler,
        config=config,
        exit_code=exit_code,
        duration_s=duration_s,
    )
    run = R.build_run_record(result, meta, log_bytes)
    text = R.render([run, *result.records])
    if out:
        out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    if not stats.ended:
        tail = f", last record was {stats.last_record}" if stats.last_record else ""
        print(f"guest stopped unexpectedly{tail}", file=sys.stderr)
    print(
        f"parsed {ndjson}: {stats.records_seen} records, "
        f"{stats.tests_seen} tests, {stats.crashes} crashes, "
        f"outcome={run['outcome']}",
        file=sys.stderr,
    )
    if stats.schema_errors:
        print(
            f"{len(stats.schema_errors)} handler(s) disagree with the kernel's "
            "declared schema",
            file=sys.stderr,
        )
        if strict_schema:
            return 2
    return 0


@ci.command
def aggregate(inputs: list[str], opts: ReportOptions | None = None) -> int:
    """Result records -> one report.

    Parameters
    ----------
    inputs
        NDJSON files or globs.
    """
    paths: list[Path] = []
    for pattern in inputs:
        matched = [Path(p) for p in sorted(glob.glob(pattern, recursive=True))]
        if not matched:
            return fail(f"no files matched: {pattern}")
        paths.extend(p for p in matched if p.is_file())
    if not paths:
        return fail("no result files matched")
    return _report(paths, opts or ReportOptions())


@ci.command
def schema(log: Path, *, out: Path | None = None) -> int:
    """A schema-dump boot -> JSON Schema.

    Parameters
    ----------
    log
        An ndjson log from a run with ndjson.schema=true.
    """
    records = S.collect(log.read_text(encoding="utf-8", errors="replace"))
    if not records:
        return fail(
            f"{log} contains no schema records (boot with ndjson.schema=true)", 1
        )
    text = json.dumps(S.build(records), indent=2, sort_keys=True) + "\n"
    if out:
        out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    print(f"described {len(records)} record types from {log}", file=sys.stderr)
    return 0


@ci.command
def protocol() -> int:
    """Print the wire names include/ndjson.h declares."""
    P.dump()
    return 0


@ci.command(name="workflow-policy")
def workflow_policy() -> int:
    """Enforce read-only Git and immutable images in execution workflows."""
    violations = WP.check()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        return fail(f"{len(violations)} workflow policy violation(s)", 1)
    print(
        f"ok: {len(WP.all_paths())} workflows checked "
        f"({len(WP.EXECUTION_WORKFLOWS)} held to the execution rules)"
    )
    return 0


@app.command
def boot(
    runs: int = 1,
    *,
    timeout: Annotated[int, Parameter(alias="-t")] = 300,
    build_dir: Annotated[Path, Parameter(alias="-B")] = Path("build"),
    out_dir: Path | None = None,
    quiet: Annotated[bool, Parameter(alias="-q")] = False,
    shard: str | None = None,
    scenario: str | None = None,
    seed: str | None = None,
    compiler: str | None = None,
    config: str | None = None,
) -> int:
    """Boot the tests target up to RUNS times, stopping at the first failure.

    Each boot's logs land in `OUT_DIR/SHARD[-N]/` and its result records in
    `OUT_DIR/SHARD[-N].ndjson`, ready for `charm ci aggregate`.

    Parameters
    ----------
    runs
        Consecutive boots.
    timeout
        Per-boot wall budget in seconds; QEMU is killed with it.
    out_dir
        Where evidence is collected (default: `BUILD_DIR/boots`).
    quiet
        Hide the build and serial output.
    shard
        Name for the result records (default: run).
    config
        Free-form guest config label.
    """
    if not (build_dir / "CMakeCache.txt").is_file():
        return fail(
            f"{build_dir} is not a configured build directory; run scripts/build.sh first"
        )
    return B.repeat(
        build_dir=build_dir,
        out_dir=out_dir or build_dir / "boots",
        runs=runs,
        timeout_s=timeout,
        quiet=quiet,
        meta=R.RunMeta(
            shard=shard,
            scenario=scenario,
            seed=seed,
            sha=os.environ.get("GITHUB_SHA"),
            compiler=compiler,
            config=config,
        ),
    )


def main(argv: Sequence[str] | None = None) -> int:
    from .nightmare.cli import app as nightmare

    args = list(sys.argv[1:] if argv is None else argv)
    # machine keeps a stdlib parser, since CMake runs it at configure time
    if args[:1] == ["machine"]:
        return MC.main(args[1:])
    if "nightmare" not in app:
        app.command(nightmare)
    try:
        result = app(args)
    except (OSError, ValueError) as error:
        return fail(str(error))
    return result if isinstance(result, int) else 0
