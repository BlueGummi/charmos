"""charm nightmare: suite TOML, the cmdline codec, and the orchestrated runs"""

import json
import os
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Annotated, Any, Literal

from cyclopts import App, Parameter

from . import aggregate as NA
from . import build_bundle as NB
from . import campaign as NCP
from . import codec as NC
from . import executor as NE
from . import grammar as NG
from . import materialize as OM
from . import planner as OP
from . import suite as NS
from . import waker as NW
from . import workflow as OW

app = App(name="nightmare", help=__doc__)

SUITE_DIR = Path("nightmare/suites")


def _json_object(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return document


def _write_json(path: Path, document: Any) -> None:
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def _instant(at: str | None) -> datetime:
    """An injected ISO-8601 clock, or now"""
    if not at:
        return datetime.now(UTC)
    value = datetime.fromisoformat(at.replace("Z", "+00:00"))
    if value.tzinfo is None:
        raise ValueError("--at must include a UTC offset")
    return value


@app.command
def validate(suites: list[Path]) -> int:
    """Load each suite TOML and report every problem."""
    failed = 0
    for path in suites:
        try:
            suite = NS.load(path)
        except NS.SuiteError as error:
            print(f"error: {error}", file=sys.stderr)
            failed += 1
            continue
        tasks = ", ".join(t.name for t in suite.tasks)
        print(f"ok {path}: {suite.meta.name} ({len(suite.tasks)} task(s): {tasks})")
    return 1 if failed else 0


@app.command
def render(
    suite: Path,
    *,
    task: str | None = None,
    boot_index: int = 0,
    campaign_id: str | None = None,
    seed: str | None = None,
    runner: int | None = None,
    out: Path | None = None,
) -> int:
    """One task and boot -> one kernel command line.

    Parameters
    ----------
    task
        Task name (default: the first).
    seed
        Base seed, decimal or 0x. Required unless the task is seedless.
    runner
        Runner index; with --seed, partitions the seed space so two runners
        never explore the same seed.
    out
        Write here instead of stdout.
    """
    model = NS.load(suite)
    chosen = model.task(task) if task else model.tasks[0]
    base = int(seed, 0) if seed is not None else None
    if base is not None and runner is not None:
        base = NC.seed_for(
            base,
            runner=runner,
            boot_index=boot_index,
            boots_per_runner=chosen.boot.max_boots,
        )
    line = NC.render(
        chosen,
        NC.BootRequest(boot_index=boot_index, campaign_id=campaign_id, seed=base),
    )
    if out:
        NC.write(out, line)
        print(f"wrote {out} ({len(line)} bytes)", file=sys.stderr)
    else:
        print(line)
    return 0


@app.command
def build(suite: Path, *, target: str = "iso") -> int:
    """The build.sh invocation for a suite."""
    print(" ".join(NC.build_command(NS.load(suite), target=target)))
    return 0


@app.command
def show(suite: Path) -> int:
    """The resolved suite, defaults applied."""
    model = NS.load(suite)
    doc = model.resolved()
    doc["build"]["command"] = NC.build_command(model)
    for entry, t in zip(doc["tasks"], model.tasks, strict=True):
        entry["max_runners"] = t.max_runners or model.meta.runners
        entry["budgets_ms"] = {
            "soft": t.boot.duration_ms,
            "guest_hard": t.boot.guest_hard_ms,
            "host_timeout": t.boot.host_timeout_ms,
        }
    print(json.dumps(doc, indent=2))
    return 0


@app.command
def run(
    suite: Path,
    *,
    runner: int = 0,
    total_runners: int | None = None,
    seed: str | None = None,
    campaign_id: str | None = None,
    budget_hours: float | None = None,
    out_dir: Path | None = None,
    build_dir: Path = Path("build"),
    gate_first: bool | None = None,
    dry_run: bool = False,
    summary: Path | None = None,
    json_out: Annotated[Path | None, Parameter(name="--json")] = None,
) -> int:
    """Execute a nightmare campaign from a suite TOML.

    Parameters
    ----------
    runner
        Runner index (0-based).
    total_runners
        Total runners (default: suite.meta.runners).
    seed
        Base seed (hex or decimal).
    budget_hours
        Override the suite budget.
    out_dir
        Where per-boot logs and reports are saved.
    gate_first
        Run the gate test first (default: from the suite).
    dry_run
        Plan and simulate without running QEMU.
    summary
        Append the markdown report here instead of printing it.
    json_out
        Write the JSON report here.
    """
    model = NS.load(suite)
    try:
        base_seed = NG.parse_uint(seed) if seed is not None else None
    except NG.GrammarError as error:
        raise ValueError(f"invalid seed {seed!r}: {error}") from None

    out_dir = out_dir or Path("campaign-results") / (campaign_id or model.meta.name)
    manifest = NCP.CampaignManifest(
        suite=model,
        runner_index=runner,
        total_runners=total_runners or model.meta.runners,
        base_seed=base_seed,
        campaign_id=campaign_id or "",
        budget_ms=int(budget_hours * 3600 * 1000) if budget_hours else 0,
        build_dir=build_dir,
        out_dir=out_dir,
        gate_first=gate_first,
        dry_run=dry_run,
    )
    result = NCP.execute(manifest)
    json_path, md_path = NCP.write_reports(result, out_dir)

    if summary:
        with summary.open("a", encoding="utf-8") as fh:
            fh.write(NCP.render_markdown(result))
    else:
        sys.stdout.write(NCP.render_markdown(result))
    if json_out:
        json_out.write_text(NCP.render_json(result), encoding="utf-8")

    print(
        f"campaign {result.campaign_id}: {result.status} (ok={result.ok}), "
        f"{result.total_boots} boot(s), {len(result.findings)} unique finding(s). "
        f"Reports: {json_path}, {md_path}",
        file=sys.stderr,
    )
    return 0 if result.ok else 1


def _execute(
    manifest: Path,
    *,
    bundle: Path | None,
    allow_development_bundle: bool,
    build_dir: Path,
    out_dir: Path,
) -> int:
    execution = NE.execute_manifest(
        manifest,
        build_dir=build_dir,
        out_dir=out_dir,
        bundle_path=bundle,
        allow_development_bundle=allow_development_bundle,
    )
    print(json.dumps(execution.document, indent=2))
    print(
        f"runner result: {execution.result_path} "
        f"({execution.document['execution']['health']})",
        file=sys.stderr,
    )
    return execution.exit_code


@app.command(name="run-manifest")
def run_manifest(
    manifest: Path,
    *,
    bundle: Path | None = None,
    allow_development_bundle: bool = False,
    build_dir: Path = Path("build"),
    out_dir: Path | None = None,
) -> int:
    """Execute one accepted runner manifest and always emit a result.

    Parameters
    ----------
    bundle
        Verified compile-once build bundle (required for real boots).
    allow_development_bundle
        Allow a prebuilt development bundle, for local proof only.
    """
    return _execute(
        manifest,
        bundle=bundle,
        allow_development_bundle=allow_development_bundle,
        build_dir=build_dir,
        out_dir=out_dir or Path("runner-results") / manifest.stem,
    )


@app.command
def replay(
    source: Path,
    *,
    bundle: Path | None = None,
    allow_development_bundle: bool = False,
    build_dir: Path = Path("build"),
    out_dir: Path | None = None,
) -> int:
    """Re-execute a manifest, prior runner result, or result directory.

    Parameters
    ----------
    bundle
        Verified compile-once build bundle (required for real boots).
    allow_development_bundle
        Allow a prebuilt development bundle, for local proof only.
    """
    manifest = NE.resolve_replay_manifest(source)
    return _execute(
        manifest,
        bundle=bundle,
        allow_development_bundle=allow_development_bundle,
        build_dir=build_dir,
        out_dir=out_dir or Path("replay-results") / manifest.stem,
    )


@app.command
def plan(
    command: Path,
    snapshot: Path,
    *,
    source_commit: str,
    runner_image: str,
    suite_dir: Path = SUITE_DIR,
    ownership: Literal["ad-hoc", "repository"] = "ad-hoc",
    out: Path | None = None,
) -> int:
    """Validate and place one batch against a fleet snapshot.

    Exits 0 only when the plan is accepted.

    Parameters
    ----------
    command
        Normalized validate_batch command JSON.
    snapshot
        Authoritative fleet snapshot JSON.
    source_commit
        Exact 40-hex source commit.
    runner_image
        Immutable GHCR image digest.
    out
        Accepted/rejected result JSON (default: stdout).
    """
    command_doc = _json_object(command)
    result = OP.Planner(suite_dir).plan(
        command_doc,
        _json_object(snapshot),
        source=OP.Source(
            command_doc.get("repository", {}).get("id", ""), source_commit
        ),
        runner_image=runner_image,
        now=datetime.now(UTC),
        ownership=ownership,
    )
    document = OP.render_result(result)
    if out:
        _write_json(out, document)
    else:
        print(json.dumps(document, indent=2))
    if isinstance(result, OP.Accepted):
        print(
            f"accepted {result.plan.id}: {len(result.plan.tasks)} manifest(s), "
            f"{len(result.plan.build_groups)} build group(s)",
            file=sys.stderr,
        )
        return 0
    if isinstance(result, OP.Stale):
        print(f"stale: current snapshot is {result.current_version}", file=sys.stderr)
    else:
        for diagnostic in result.diagnostics:
            print(f"{diagnostic['field']}: {diagnostic['message']}", file=sys.stderr)
    return 2


@app.command(name="build-bundle")
def build_bundle(
    plan: Path,
    *,
    group: str,
    build_dir: Path,
    out_dir: Path,
    receipt: Path | None = None,
    prebuilt: bool = False,
) -> int:
    """Compile one accepted build group into a bundle.

    Parameters
    ----------
    plan
        Accepted plan JSON.
    group
        Accepted build group ID.
    receipt
        Write the verified receipt JSON here.
    prebuilt
        Package an existing build directory without compiling (development only).
    """
    bundle = NB.create_bundle(
        NB.request_from_plan(_json_object(plan), group),
        build_dir=build_dir,
        out_dir=out_dir,
        compile_kernel=not prebuilt,
    )
    document = NB.receipt(bundle)
    if receipt:
        _write_json(receipt, document)
    print(json.dumps(document, indent=2))
    return 0


@app.command
def materialize(
    plan: Path,
    receipts: list[Path],
    *,
    out_dir: Path,
    attempt: int = 1,
    dry_run: bool = False,
) -> int:
    """Accepted plan plus build receipts -> runner manifests.

    Parameters
    ----------
    plan
        Accepted plan JSON.
    receipts
        Verified bundle.json receipts.
    """
    bundle = OM.materialize(
        OM.load_plan(plan),
        tuple(OM.load_receipt(path) for path in receipts),
        attempt=attempt,
        dry_run=dry_run,
    )
    print(OM.write_bundle(bundle, out_dir))
    return 0


def _planner_inputs(
    command: dict[str, Any],
    snapshot: dict[str, Any],
    command_out: Path,
    snapshot_out: Path,
) -> int:
    _write_json(command_out, command)
    _write_json(snapshot_out, snapshot)
    print(f"{command['command_id']} {snapshot['version']}")
    return 0


@app.command(name="repository-command")
def repository_command(
    *,
    suite: str,
    repository: str,
    command_out: Path,
    snapshot_out: Path,
    ref: str = "main",
    suite_dir: Path = SUITE_DIR,
    runner_capacity: int = 12,
    at: str | None = None,
) -> int:
    """Create trusted planner inputs for one committed repository suite.

    Parameters
    ----------
    at
        Injected ISO-8601 clock.
    """
    command, snapshot = OW.repository_command(
        suite_id=suite,
        suite_dir=suite_dir,
        repository=repository,
        ref=ref,
        now=_instant(at),
        runner_capacity=runner_capacity,
    )
    return _planner_inputs(command, snapshot, command_out, snapshot_out)


@app.command(name="inline-command")
def inline_command(
    *,
    toml: str,
    repository: str,
    command_out: Path,
    snapshot_out: Path,
    ref: str = "main",
    runner_capacity: int = 12,
    at: str | None = None,
) -> int:
    """Create trusted planner inputs for one inline TOML batch definition.

    Parameters
    ----------
    toml
        Raw batch TOML definition string.
    at
        Injected ISO-8601 clock.
    """
    command, snapshot = OW.inline_command(
        toml_text=toml,
        repository=repository,
        ref=ref,
        now=_instant(at),
        runner_capacity=runner_capacity,
    )
    return _planner_inputs(command, snapshot, command_out, snapshot_out)


@app.command
def matrix(kind: Literal["build", "runner"], document: Path, *, limit: int = 64) -> int:
    """Emit a bounded trusted Actions matrix."""
    source = _json_object(document)
    rows = (
        OW.build_matrix(source, limit)
        if kind == "build"
        else OW.runner_matrix(source, limit)
    )
    print(json.dumps({"include": rows}, separators=(",", ":")))
    return 0


@app.command(name="aggregate")
def aggregate_(
    plan: Path,
    results_dir: Path,
    *,
    json_out: Annotated[Path, Parameter(name="--json")],
    markdown: Path,
    plan_bundle: Path | None = None,
    builds_dir: Path | None = None,
    summary: Path | None = None,
) -> int:
    """Verify completeness, identity, discovery, and infrastructure.

    Parameters
    ----------
    plan
        Accepted plan result JSON.
    """
    report = NA.aggregate(
        _json_object(plan),
        results_dir=results_dir,
        plan_bundle_path=plan_bundle,
        builds_dir=builds_dir,
    )
    NA.write(report, json_path=json_out, markdown_path=markdown)
    text = NA.render_markdown(report)
    if summary:
        with summary.open("a", encoding="utf-8") as handle:
            handle.write(text)
    sys.stdout.write(text)
    return 0 if report.ok else 1


@app.command(name="queue-command")
def queue_command(
    command: Path, *, batch_id: str, source_sha: str, out: Path, at: str | None = None
) -> int:
    """Classify one accepted command for deferred execution; prints true/false.

    Parameters
    ----------
    at
        Injected ISO-8601 clock.
    """
    metadata = OW.queue_metadata(
        _json_object(command),
        batch_id=batch_id,
        source_sha=source_sha,
        now=_instant(at),
    )
    _write_json(out, metadata)
    print("true" if metadata["deferred"] else "false")
    return 0


@app.command(name="wake-queue")
def wake_queue(
    *,
    repository: str,
    workflow: str = "nightmare-orchestrator.yml",
    ref: str = "main",
    api_url: str = "https://api.github.com",
    token_env: str = "GITHUB_TOKEN",
) -> int:
    """Dispatch all due artifact-backed Nightmare plans."""
    due = NW.wake_due(
        token=os.environ.get(token_env, ""),
        repository=repository,
        workflow=workflow,
        ref=ref,
        api_url=api_url,
    )
    for queue in due:
        print(f"dispatched {queue.batch_id} from queued run {queue.run_id}")
    if not due:
        print("no deferred Nightmare plans are due")
    return 0
