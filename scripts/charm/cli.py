"""``charm`` the entry point for charmOS host tooling

Subcommand groups mirror the stage they serve:

    charm ci ...
    charm dev ...
    charm nightmare ...
"""

import argparse
import glob
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from . import ingest as I
from . import protocol as P
from . import record as R
from . import report as RP


def _expand(patterns: list[str]) -> tuple[list[Path], list[str]]:
    paths: list[Path] = []
    unmatched: list[str] = []
    for pattern in patterns:
        expanded = [Path(p) for p in sorted(glob.glob(pattern))]
        if expanded:
            paths.extend(p for p in expanded if p.is_file())
        else:
            unmatched.append(pattern)
    return paths, unmatched


def _write_report(rep: dict[str, Any], args: argparse.Namespace, run_count: int) -> int:
    markdown = RP.render_markdown(rep)
    if args.summary:
        with Path(args.summary).open("a", encoding="utf-8") as fh:
            fh.write(markdown)
    sys.stdout.write(markdown)

    if args.json:
        Path(args.json).write_text(RP.render_json(rep), encoding="utf-8")

    print(
        f"{run_count} shard(s), {len(rep['failed_runs'])} failed, "
        f"{len(rep['unique_crashes'])} unique crash(es)",
        file=sys.stderr,
    )
    return 1 if (args.fail_on_error and not rep["ok"]) else 0


def cmd_parse(args: argparse.Namespace) -> int:
    try:
        log_bytes = Path(args.log).stat().st_size
    except FileNotFoundError:
        log_bytes = 0

    try:
        with Path(args.ndjson).open(encoding="utf-8", errors="replace") as fh:
            result = R.parse_ndjson(fh)
    except FileNotFoundError:
        print(f"error: {args.ndjson} not found", file=sys.stderr)
        return 2

    stats = result.stats

    for err in stats.schema_errors:
        print(f"schema drift: {err}", file=sys.stderr)
        result.records.append({"type": "schema_error", "detail": err})

    if not stats.schema_seen:
        print(
            f"warning: {args.ndjson} contains no schema records (boot with ndjson.schema=true)",
            file=sys.stderr,
        )

    meta = R.RunMeta(
        shard=args.shard,
        scenario=args.scenario,
        seed=args.seed,
        sha=args.sha,
        compiler=args.compiler,
        config=args.config,
        exit_code=args.exit_code,
        duration_s=args.duration_s,
    )
    run = R.build_run_record(result, meta, log_bytes)
    text = R.render([run, *result.records])

    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    if not stats.ended:
        tail = f", last record was {stats.last_record}" if stats.last_record else ""
        print(
            f"guest stopped unexpectedly{tail}",
            file=sys.stderr,
        )

    print(
        f"parsed {args.ndjson}: {stats.records_seen} records, "
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
        if args.strict_schema:
            return 2

    return 0


def cmd_aggregate(args: argparse.Namespace) -> int:
    paths, unmatched = _expand(args.inputs)
    if unmatched:
        print(f"error: no files matched: {', '.join(unmatched)}", file=sys.stderr)
        return 2
    if not paths:
        print("error: no result files matched", file=sys.stderr)
        return 2

    runs, tests, crashes, stale = RP.load(paths)
    if not runs:
        print(f"error: no run records found in {len(paths)} file(s)", file=sys.stderr)
        return 2

    rep = RP.build_report(runs, tests, crashes, args.repro_template)
    for path in stale:
        rep["warnings"].insert(
            0, f"{RP.md_code(path)} was written by a different result version"
        )
    return _write_report(rep, args, len(runs))


def cmd_ingest(args: argparse.Namespace) -> int:
    root = Path(args.artifacts)
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 2

    written, problems = I.ingest(root, Path(args.ndjson_dir))
    I.report_problems(problems)

    if not written:
        print(f"error: no shard produced result records under {root}", file=sys.stderr)
        return 2

    runs, tests, crashes, stale = RP.load(written)
    if not runs:
        print(f"error: no run records in {len(written)} file(s)", file=sys.stderr)
        return 2

    rep = RP.build_report(runs, tests, crashes, args.repro_template)
    for path in stale:
        rep["warnings"].insert(
            0, f"{RP.md_code(path)} was written by a different result version"
        )
    for p in problems:
        if p.level == "error":
            rep["warnings"].insert(0, RP.md_escape(p.text))

    return _write_report(rep, args, len(runs))


def cmd_schema(args: argparse.Namespace) -> int:
    from . import schema as S

    log_path = Path(args.log)
    records = S.collect(log_path.read_text(encoding="utf-8", errors="replace"))
    if not records:
        print(
            f"error: {log_path} contains no schema records (boot with ndjson.schema=true)",
            file=sys.stderr,
        )
        return 1

    import json

    text = json.dumps(S.build(records), indent=2, sort_keys=True) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    print(f"described {len(records)} record types from {log_path}", file=sys.stderr)
    return 0


def cmd_protocol(_args: argparse.Namespace) -> int:
    P.dump()
    return 0


def cmd_workflow_policy(_args: argparse.Namespace) -> int:
    from . import workflow_policy as WP

    violations = WP.check()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(
            f"error: {len(violations)} workflow policy violation(s)",
            file=sys.stderr,
        )
        return 1
    print(f"ok: {len(WP.PROTECTED_WORKFLOWS)} protected workflows")
    return 0


def cmd_repeat(args: argparse.Namespace) -> int:
    from . import local as L

    build_dir = Path(args.build_dir)
    if not (build_dir / "CMakeCache.txt").is_file():
        print(
            f"error: {build_dir} is not a configured build directory; "
            "run scripts/build.sh first",
            file=sys.stderr,
        )
        return 2

    return L.repeat(
        build_dir=build_dir,
        results_root=Path(args.results_dir or build_dir / "repeated-test-runs"),
        runs=args.runs,
        timeout_s=args.timeout,
        target=args.target,
    )


def _load_suite(path_str: str) -> Any | None:
    from .nightmare import suite as NS

    try:
        return NS.load(Path(path_str))
    except NS.SuiteError as e:
        print(f"error: {e}", file=sys.stderr)
        return None


def cmd_nm_validate(args: argparse.Namespace) -> int:
    from .nightmare import suite as NS

    failed = 0
    for path_str in args.suites:
        suite = _load_suite(path_str)
        if suite is None:
            failed += 1
            continue
        tasks = ", ".join(t.name for t in suite.tasks)
        print(f"ok {path_str}: {suite.meta.name} ({len(suite.tasks)} task(s): {tasks})")

    if not NS.SCHEMA_AVAILABLE:
        print(
            f"note: jsonschema is not installed; schema validation for {NS.SCHEMA_PATH.name} skipped",
            file=sys.stderr,
        )
        if args.require_schema:
            return 2

    return 1 if failed else 0


def cmd_nm_render(args: argparse.Namespace) -> int:
    from .nightmare import codec as NC

    suite = _load_suite(args.suite)
    if suite is None:
        return 2
    try:
        task = suite.task(args.task) if args.task else suite.tasks[0]
    except KeyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    seed = None
    if args.seed is not None:
        seed = int(args.seed, 0)
        if args.runner is not None:
            seed = NC.seed_for(
                seed,
                runner=args.runner,
                boot_index=args.boot_index,
                boots_per_runner=task.boot.max_boots,
            )

    try:
        line = NC.render(
            task,
            NC.BootRequest(
                boot_index=args.boot_index,
                campaign_id=args.campaign_id,
                seed=seed,
            ),
        )
    except NC.CodecError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if args.out:
        NC.write(Path(args.out), line)
        print(f"wrote {args.out} ({len(line)} bytes)", file=sys.stderr)
    else:
        print(line)
    return 0


def cmd_nm_build(args: argparse.Namespace) -> int:
    from .nightmare import codec as NC

    suite = _load_suite(args.suite)
    if suite is None:
        return 2
    print(" ".join(NC.build_command(suite, target=args.target)))
    return 0


def cmd_nm_show(args: argparse.Namespace) -> int:
    import json

    from .nightmare import codec as NC

    suite = _load_suite(args.suite)
    if suite is None:
        return 2

    doc = {
        "suite": {
            "name": suite.meta.name,
            "runners": suite.meta.runners,
            "budget_hours": suite.meta.budget_hours,
            "overlap_ratio": suite.meta.overlap_ratio,
        },
        "build": {
            "compiler": suite.build.compiler,
            "type": suite.build.type,
            "cmake_definitions": list(suite.build.cmake_definitions),
            "smp": suite.build.smp.topo(),
            "memory_mib": suite.build.memory_mib,
            "command": NC.build_command(suite),
        },
        "tasks": [
            {
                "name": t.name,
                "mode": t.mode,
                "weight": t.weight,
                "priority": t.priority,
                "max_runners": t.max_runners or suite.meta.runners,
                "budgets_ms": {
                    "soft": t.boot.duration_ms,
                    "guest_hard": t.boot.guest_hard_ms,
                    "host_timeout": t.boot.host_timeout_ms,
                },
                "max_boots": t.boot.max_boots,
                "seed_mode": t.nightmare.seed_mode,
                "perturb": list(t.nightmare.perturb),
            }
            for t in suite.tasks
        ],
    }
    print(json.dumps(doc, indent=2))
    return 0


def cmd_nm_run(args: argparse.Namespace) -> int:
    from .nightmare import campaign as NC
    from .nightmare import grammar as NG

    suite = _load_suite(args.suite)
    if suite is None:
        return 2

    base_seed = None
    if args.seed is not None:
        try:
            base_seed = NG.parse_uint(args.seed)
        except Exception as e:
            print(f"error: invalid seed {args.seed!r}: {e}", file=sys.stderr)
            return 2

    out_dir = (
        Path(args.out_dir)
        if args.out_dir
        else Path("campaign-results") / (args.campaign_id or suite.meta.name)
    )
    build_dir = Path(args.build_dir)

    manifest = NC.CampaignManifest(
        suite=suite,
        runner_index=args.runner,
        total_runners=args.total_runners or suite.meta.runners,
        base_seed=base_seed,
        campaign_id=args.campaign_id or "",
        budget_ms=int(args.budget_hours * 3600 * 1000) if args.budget_hours else 0,
        build_dir=build_dir,
        out_dir=out_dir,
        gate_first=args.gate_first,
        dry_run=args.dry_run,
    )

    result = NC.execute(manifest)
    json_path, md_path = NC.write_reports(result, out_dir)

    if args.summary:
        with Path(args.summary).open("a", encoding="utf-8") as fh:
            fh.write(NC.render_markdown(result))
    else:
        sys.stdout.write(NC.render_markdown(result))

    if args.json:
        Path(args.json).write_text(NC.render_json(result), encoding="utf-8")

    print(
        f"campaign {result.campaign_id}: {result.status} (ok={result.ok}), "
        f"{result.total_boots} boot(s), {len(result.findings)} unique finding(s). "
        f"Reports: {json_path}, {md_path}",
        file=sys.stderr,
    )

    return 0 if result.ok else 1


def _execute_runner_manifest(
    manifest_path: Path,
    *,
    build_dir: Path,
    out_dir: Path,
    bundle_path: Path | None = None,
    allow_development_bundle: bool = False,
) -> int:
    import json

    from .nightmare import executor as NE

    execution = NE.execute_manifest(
        manifest_path,
        build_dir=build_dir,
        out_dir=out_dir,
        bundle_path=bundle_path,
        allow_development_bundle=allow_development_bundle,
    )
    print(json.dumps(execution.document, indent=2))
    print(
        f"runner result: {execution.result_path} "
        f"({execution.document['execution']['health']})",
        file=sys.stderr,
    )
    return execution.exit_code


def cmd_nm_run_manifest(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest)
    out_dir = Path(args.out_dir or Path("runner-results") / manifest_path.stem)
    return _execute_runner_manifest(
        manifest_path,
        build_dir=Path(args.build_dir),
        out_dir=out_dir,
        bundle_path=Path(args.bundle) if args.bundle else None,
        allow_development_bundle=args.allow_development_bundle,
    )


def cmd_nm_replay(args: argparse.Namespace) -> int:
    from .nightmare import contracts as NCT
    from .nightmare import executor as NE

    try:
        manifest_path = NE.resolve_replay_manifest(Path(args.source))
    except NCT.ContractError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir or Path("replay-results") / manifest_path.stem)
    return _execute_runner_manifest(
        manifest_path,
        build_dir=Path(args.build_dir),
        out_dir=out_dir,
        bundle_path=Path(args.bundle) if args.bundle else None,
        allow_development_bundle=args.allow_development_bundle,
    )


def _read_json_object(path: Path) -> dict[str, Any]:
    import json

    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return document


def cmd_nm_plan(args: argparse.Namespace) -> int:
    import json
    from datetime import UTC, datetime

    from .orchestrator import domain as OD
    from .orchestrator import planner as OP

    try:
        command = _read_json_object(Path(args.command))
        snapshot = _read_json_object(Path(args.snapshot))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    source = OD.Source(command.get("repository", {}).get("id", ""), args.source_commit)
    result = OP.Planner(Path(args.suite_dir)).plan(
        command,
        snapshot,
        source=source,
        runner_image=args.runner_image,
        now=datetime.now(UTC),
        ownership=args.ownership,
    )
    document = OP.render_result(result)
    text = json.dumps(document, indent=2) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    if isinstance(result, OD.Accepted):
        print(
            f"accepted {result.plan.id}: {len(result.plan.tasks)} manifest(s), "
            f"{len(result.plan.build_groups)} build group(s)",
            file=sys.stderr,
        )
        return 0
    if isinstance(result, OD.Stale):
        print(f"stale: current snapshot is {result.current_version}", file=sys.stderr)
    else:
        for diagnostic in result.diagnostics:
            print(f"{diagnostic['field']}: {diagnostic['message']}", file=sys.stderr)
    return 2


def cmd_nm_build_bundle(args: argparse.Namespace) -> int:
    import json

    from .nightmare import build_bundle as NB

    try:
        plan = _read_json_object(Path(args.plan))
        request = NB.request_from_plan(plan, args.group)
        bundle = NB.create_bundle(
            request,
            build_dir=Path(args.build_dir),
            out_dir=Path(args.out_dir),
            compile_kernel=not args.prebuilt,
        )
    except (OSError, ValueError, json.JSONDecodeError, NB.BundleError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    receipt = NB.receipt(bundle)
    if args.receipt:
        Path(args.receipt).write_text(
            json.dumps(receipt, indent=2) + "\n", encoding="utf-8"
        )
    print(json.dumps(receipt, indent=2))
    return 0


def cmd_nm_materialize(args: argparse.Namespace) -> int:
    from .orchestrator import materialize as OM

    try:
        plan = OM.load_plan(Path(args.plan))
        receipts = tuple(OM.load_receipt(Path(path)) for path in args.receipts)
        bundle = OM.materialize(
            plan, receipts, attempt=args.attempt, dry_run=args.dry_run
        )
        index = OM.write_bundle(bundle, Path(args.out_dir))
    except OM.MaterializationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(index)
    return 0


def cmd_nm_repack_bundle(args: argparse.Namespace) -> int:
    import json

    from .nightmare import build_bundle as NB

    try:
        bundle = NB.verify_bundle(Path(args.bundle))
        measurement = NB.repack(
            bundle,
            cmdline=Path(args.cmdline),
            out_dir=Path(args.out_dir),
        )
        transport = NB.measure_transport(bundle, Path(args.out_dir) / "measurement")
    except NB.BundleError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                **transport,
                "repack_ms": round(measurement.repack_ms, 3),
                "iso_size_bytes": measurement.iso_size_bytes,
                "iso": str(measurement.iso_path),
            },
            indent=2,
        )
    )
    return 0


def cmd_nm_repository_command(args: argparse.Namespace) -> int:
    import json
    from datetime import UTC, datetime

    from .orchestrator import workflow as OW

    try:
        at = (
            datetime.fromisoformat(args.at.replace("Z", "+00:00"))
            if args.at
            else datetime.now(UTC)
        )
        if at.tzinfo is None:
            raise ValueError("--at must include a UTC offset")
        command, snapshot = OW.repository_command(
            suite_id=args.suite,
            suite_dir=Path(args.suite_dir),
            repository=args.repository,
            ref=args.ref,
            now=at,
            runner_capacity=args.runner_capacity,
        )
        Path(args.command_out).write_text(
            json.dumps(command, indent=2) + "\n", encoding="utf-8"
        )
        Path(args.snapshot_out).write_text(
            json.dumps(snapshot, indent=2) + "\n", encoding="utf-8"
        )
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"{command['command_id']} {snapshot['version']}")
    return 0


def cmd_nm_matrix(args: argparse.Namespace) -> int:
    import json

    from .orchestrator import workflow as OW

    try:
        document = _read_json_object(Path(args.document))
        rows = (
            OW.build_matrix(document, args.limit)
            if args.kind == "build"
            else OW.runner_matrix(document, args.limit)
        )
    except (OSError, ValueError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"include": rows}, separators=(",", ":")))
    return 0


def cmd_nm_aggregate(args: argparse.Namespace) -> int:
    import json

    from .nightmare import aggregate as NA

    try:
        accepted_plan = _read_json_object(Path(args.plan))
        report = NA.aggregate(
            accepted_plan,
            results_dir=Path(args.results_dir),
            plan_bundle_path=Path(args.plan_bundle) if args.plan_bundle else None,
            builds_dir=Path(args.builds_dir) if args.builds_dir else None,
        )
        NA.write(
            report,
            json_path=Path(args.json),
            markdown_path=Path(args.markdown),
        )
        markdown = NA.render_markdown(report)
        if args.summary:
            with Path(args.summary).open("a", encoding="utf-8") as handle:
                handle.write(markdown)
        sys.stdout.write(markdown)
    except (OSError, ValueError, json.JSONDecodeError, NA.AggregateError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0 if report.ok else 1


def cmd_nm_recover(args: argparse.Namespace) -> int:
    import json
    import os

    from .orchestrator import reconciler as OR
    from .orchestrator.coordinator import Coordinator
    from .orchestrator.store import PostgresStore
    from .orchestrator.transport import GitHubActionsTransport

    dsn = os.environ.get(args.dsn_env, "")
    token = os.environ.get(args.token_env, "")
    if not dsn or not token:
        print(
            f"error: {args.dsn_env} and {args.token_env} must be set",
            file=sys.stderr,
        )
        return 2
    try:
        store = PostgresStore.from_dsn(dsn)
        store.migrate()
        transport = GitHubActionsTransport(
            repository=args.repository,
            workflow=args.workflow,
            ref=args.ref,
            token=token,
        )
        coordinator = Coordinator(store, transport)
        coordinator.deliver()
        result = coordinator.apply(OR.Tick(idle_grace_ms=args.idle_grace_ms))
    except (RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result.result, indent=2))
    return 0


def cmd_nm_prepare_execution(args: argparse.Namespace) -> int:
    import json

    from .orchestrator.http import create_operator_from_env
    from .orchestrator.runtime import ExecutionRuntime, RuntimeError

    try:
        prepared = ExecutionRuntime(create_operator_from_env()).prepare(
            wake_id=args.wake_id,
            command_ref=args.command_ref,
            owner=args.owner,
            lease_ms=args.lease_ms,
        )
        outputs = {
            Path(args.command_out): prepared.command,
            Path(args.snapshot_out): prepared.snapshot,
            Path(args.plan_out): prepared.plan,
            Path(args.attempt_out): {
                "attempt_id": prepared.attempt_id,
                "owner": args.owner,
            },
        }
        for path, document in outputs.items():
            path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(prepared.attempt_id)
    return 0


def cmd_nm_finish_execution(args: argparse.Namespace) -> int:
    import json

    from .nightmare import contracts
    from .orchestrator.http import create_operator_from_env
    from .orchestrator.runtime import ExecutionRuntime, RuntimeError

    try:
        attempt = _read_json_object(Path(args.attempt))
        report = _read_json_object(Path(args.report))
        digests: dict[str, str] = {}
        for path in sorted(Path(args.results_dir).rglob("runner_result.json")):
            result = _read_json_object(path)
            manifest_id = result.get("manifest_id")
            if isinstance(manifest_id, str):
                digest = contracts.sha256_file(path)
                prior = digests.setdefault(manifest_id, digest)
                if prior != digest:
                    raise ValueError(f"multiple result digests exist for {manifest_id}")
        result = ExecutionRuntime(create_operator_from_env()).finish(
            attempt_id=str(attempt["attempt_id"]),
            report=report,
            result_digests=digests,
            owner=str(attempt["owner"]),
        )
    except (KeyError, OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


def cmd_nm_serve(args: argparse.Namespace) -> int:
    from wsgiref.simple_server import make_server

    from .orchestrator.http import create_application_from_env

    try:
        application = create_application_from_env()
    except (RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"serving Nightmare orchestrator on http://{args.host}:{args.port}")
    with make_server(args.host, args.port, application) as server:
        server.serve_forever()
    return 0


def add_report_args(sp: argparse.ArgumentParser) -> None:
    sp.add_argument("--summary", help="write markdown here (append if exists)")
    sp.add_argument("--json", help="write merged JSON report here")
    sp.add_argument(
        "--repro-template",
        default="",
        help="shell snippet shown for reproducing a crash, with {seed} and "
        "{scenario} substituted",
    )
    sp.add_argument(
        "--fail-on-error",
        action="store_true",
        help="exit 1 if any shard failed (gate jobs want this; hunt jobs "
        "generally do not, since a finding is an issue and not a red X)",
    )


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="charm", description=__doc__)
    groups = ap.add_subparsers(dest="group", required=True)

    ci = groups.add_parser("ci", help="CI result parsing and reporting")
    sub = ci.add_subparsers(dest="command", required=True)

    p = sub.add_parser("parse", help="one run's logs -> result records")
    p.add_argument("log", help="QEMU serial log, kept for its size only")
    p.add_argument("--ndjson", required=True, help="the kernel's machine channel")
    p.add_argument(
        "--strict-schema",
        action="store_true",
        help="exit non-zero when a handler disagrees with the emitted schema",
    )
    p.add_argument("--out", help="NDJSON output (default: stdout)")
    p.add_argument("--shard", default=None)
    p.add_argument("--scenario", default=None)
    p.add_argument("--seed", default=None)
    p.add_argument("--sha", default=None)
    p.add_argument("--compiler", default=None)
    p.add_argument("--config", default=None, help="free-form guest config label")
    p.add_argument("--duration-s", type=float, default=None)
    p.add_argument(
        "--exit-code",
        type=int,
        default=None,
        help="exit status of the QEMU wrapper (0 pass, 1 test failure, "
        "3 panic, 124 timeout by convention)",
    )
    p.set_defaults(fn=cmd_parse)

    a = sub.add_parser("aggregate", help="result records -> one report")
    a.add_argument("inputs", nargs="+", help="NDJSON files or globs")
    add_report_args(a)
    a.set_defaults(fn=cmd_aggregate)

    g = sub.add_parser(
        "ingest",
        help="a downloaded artifact tree -> result records -> one report",
    )
    g.add_argument("artifacts", help="directory holding the downloaded artifacts")
    g.add_argument(
        "--ndjson-dir",
        default="ndjson",
        help="where per-shard result records are written (default: ndjson)",
    )
    add_report_args(g)
    g.set_defaults(fn=cmd_ingest)

    s = sub.add_parser("schema", help="a schema-dump boot -> JSON Schema")
    s.add_argument("log", help="an ndjson log from a run with ndjson.schema=true")
    s.add_argument("--out", help="output path (default: stdout)")
    s.set_defaults(fn=cmd_schema)

    n = sub.add_parser(
        "protocol", help="print the wire names include/ndjson.h declares"
    )
    n.set_defaults(fn=cmd_protocol)

    wp = sub.add_parser(
        "workflow-policy",
        help="enforce read-only Git and immutable images in execution workflows",
    )
    wp.set_defaults(fn=cmd_workflow_policy)

    dev = groups.add_parser("dev", help="loops against a local build")
    devsub = dev.add_subparsers(dest="command", required=True)

    r = devsub.add_parser(
        "repeat", help="build the test target N times, stopping at the first failure"
    )
    r.add_argument(
        "runs", nargs="?", type=int, default=1, help="iterations (default 1)"
    )
    r.add_argument(
        "-t",
        "--timeout",
        type=int,
        default=60,
        help="per-iteration wall budget in seconds (default 60)",
    )
    r.add_argument("-B", "--build-dir", default="build", help="build directory")
    r.add_argument("--target", default="tests", help="build target (default: tests)")
    r.add_argument(
        "--results-dir",
        default=None,
        help="where per-run evidence is collected "
        "(default: <build-dir>/repeated-test-runs)",
    )
    r.set_defaults(fn=cmd_repeat)

    nm = groups.add_parser("nightmare", help="suite TOML and the cmdline codec")
    nmsub = nm.add_subparsers(dest="command", required=True)

    v = nmsub.add_parser("validate", help="load a suite and report every problem")
    v.add_argument("suites", nargs="+", help="suite TOML files")
    v.add_argument(
        "--require-schema",
        action="store_true",
        help="fail if jsonschema is unavailable, so CI cannot silently skip the "
        "published contract",
    )
    v.set_defaults(fn=cmd_nm_validate)

    rd = nmsub.add_parser("render", help="one task and boot -> one kernel command line")
    rd.add_argument("suite", help="suite TOML file")
    rd.add_argument("--task", default=None, help="task name (default: the first)")
    rd.add_argument("--boot-index", type=int, default=0)
    rd.add_argument("--campaign-id", default=None)
    rd.add_argument(
        "--seed",
        default=None,
        help="base seed, decimal or 0x. Required unless the task is seedless",
    )
    rd.add_argument(
        "--runner",
        type=int,
        default=None,
        help="runner index; with --seed, partitions the seed space so two "
        "runners never explore the same seed",
    )
    rd.add_argument("--out", default=None, help="write here instead of stdout")
    rd.set_defaults(fn=cmd_nm_render)

    bl = nmsub.add_parser("build", help="the build.sh invocation for a suite")
    bl.add_argument("suite", help="suite TOML file")
    bl.add_argument("--target", default="iso")
    bl.set_defaults(fn=cmd_nm_build)

    sh = nmsub.add_parser("show", help="the resolved suite, defaults applied")
    sh.add_argument("suite", help="suite TOML file")
    sh.set_defaults(fn=cmd_nm_show)

    rn = nmsub.add_parser("run", help="execute a nightmare campaign from a suite TOML")
    rn.add_argument("suite", help="suite TOML file")
    rn.add_argument("--runner", type=int, default=0, help="runner index (0-based)")
    rn.add_argument(
        "--total-runners",
        type=int,
        default=None,
        help="total runners (default: suite.meta.runners)",
    )
    rn.add_argument(
        "--seed",
        default=None,
        help="base seed (hex or decimal)",
    )
    rn.add_argument(
        "--campaign-id",
        default=None,
        help="campaign identifier (default: <suite>-<timestamp>)",
    )
    rn.add_argument(
        "--budget-hours",
        type=float,
        default=None,
        help="override suite budget in hours",
    )
    rn.add_argument(
        "--out-dir",
        default=None,
        help="directory where per-boot logs and reports are saved",
    )
    rn.add_argument(
        "--build-dir",
        default="build",
        help="charmOS build directory (default: build)",
    )
    rn.add_argument(
        "--gate-first",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="run gate test first (default: from suite)",
    )
    rn.add_argument(
        "--dry-run",
        action="store_true",
        help="plan and simulate without running QEMU",
    )
    add_report_args(rn)
    rn.set_defaults(fn=cmd_nm_run)

    pl = nmsub.add_parser(
        "plan", help="validate and place one batch against a fleet snapshot"
    )
    pl.add_argument("command", help="normalized validate_batch command JSON")
    pl.add_argument("snapshot", help="authoritative fleet snapshot JSON")
    pl.add_argument("--source-commit", required=True, help="exact 40-hex source commit")
    pl.add_argument("--runner-image", required=True, help="immutable GHCR image digest")
    pl.add_argument("--suite-dir", default="nightmare/suites")
    pl.add_argument("--ownership", choices=("ad-hoc", "repository"), default="ad-hoc")
    pl.add_argument("--out", default=None, help="accepted/rejected result JSON")
    pl.set_defaults(fn=cmd_nm_plan)

    bb = nmsub.add_parser(
        "build-bundle", help="compile one accepted build group into a bundle"
    )
    bb.add_argument("plan", help="accepted plan JSON")
    bb.add_argument("--group", required=True, help="accepted build group ID")
    bb.add_argument("--build-dir", required=True)
    bb.add_argument("--out-dir", required=True)
    bb.add_argument(
        "--receipt", default=None, help="write the verified receipt JSON here"
    )
    bb.add_argument(
        "--prebuilt",
        action="store_true",
        help="package an existing build directory without compiling (development only)",
    )
    bb.set_defaults(fn=cmd_nm_build_bundle)

    mt = nmsub.add_parser(
        "materialize", help="accepted plan plus build receipts to runner manifests"
    )
    mt.add_argument("plan", help="accepted plan JSON")
    mt.add_argument("receipts", nargs="+", help="verified bundle.json receipts")
    mt.add_argument("--out-dir", required=True)
    mt.add_argument("--attempt", type=int, default=1)
    mt.add_argument("--dry-run", action="store_true")
    mt.set_defaults(fn=cmd_nm_materialize)

    rb = nmsub.add_parser(
        "repack-bundle", help="repack a verified bundle with one kernel command line"
    )
    rb.add_argument("bundle", help="build bundle directory")
    rb.add_argument("cmdline", help="kernel command-line file")
    rb.add_argument("--out-dir", required=True)
    rb.set_defaults(fn=cmd_nm_repack_bundle)

    rc = nmsub.add_parser(
        "repository-command",
        help="create trusted planner inputs for one committed repository suite",
    )
    rc.add_argument("--suite", required=True)
    rc.add_argument("--repository", required=True)
    rc.add_argument("--ref", default="main")
    rc.add_argument("--suite-dir", default="nightmare/suites")
    rc.add_argument("--runner-capacity", type=int, default=12)
    rc.add_argument("--at", default=None, help="injected ISO-8601 clock")
    rc.add_argument("--command-out", required=True)
    rc.add_argument("--snapshot-out", required=True)
    rc.set_defaults(fn=cmd_nm_repository_command)

    mx = nmsub.add_parser("matrix", help="emit a bounded trusted Actions matrix")
    mx.add_argument("kind", choices=("build", "runner"))
    mx.add_argument("document")
    mx.add_argument("--limit", type=int, default=64)
    mx.set_defaults(fn=cmd_nm_matrix)

    ag = nmsub.add_parser(
        "aggregate", help="verify completeness, identity, discovery, and infrastructure"
    )
    ag.add_argument("plan", help="accepted plan result JSON")
    ag.add_argument("results_dir")
    ag.add_argument("--plan-bundle", default=None)
    ag.add_argument("--builds-dir", default=None)
    ag.add_argument("--json", required=True)
    ag.add_argument("--markdown", required=True)
    ag.add_argument("--summary", default=None)
    ag.set_defaults(fn=cmd_nm_aggregate)

    rv = nmsub.add_parser(
        "recover", help="repair expired coordinator leases and undelivered wakes"
    )
    rv.add_argument("--repository", required=True)
    rv.add_argument("--workflow", default="nightmare-orchestrator.yml")
    rv.add_argument("--ref", default="main")
    rv.add_argument("--dsn-env", default="NIGHTMARE_DATABASE_DSN")
    rv.add_argument("--token-env", default="GITHUB_TOKEN")
    rv.add_argument("--idle-grace-ms", type=int, default=30_000)
    rv.set_defaults(fn=cmd_nm_recover)

    pe = nmsub.add_parser(
        "prepare-execution",
        help="acquire one durable wake and export its accepted execution plan",
    )
    pe.add_argument("--wake-id", required=True)
    pe.add_argument("--command-ref", required=True)
    pe.add_argument("--owner", required=True)
    pe.add_argument("--lease-ms", type=int, default=30 * 60 * 1000)
    pe.add_argument("--command-out", required=True)
    pe.add_argument("--snapshot-out", required=True)
    pe.add_argument("--plan-out", required=True)
    pe.add_argument("--attempt-out", required=True)
    pe.set_defaults(fn=cmd_nm_prepare_execution)

    fe = nmsub.add_parser(
        "finish-execution",
        help="retain aggregate lifecycle and release one durable coordinator",
    )
    fe.add_argument("--attempt", required=True)
    fe.add_argument("--report", required=True)
    fe.add_argument("--results-dir", required=True)
    fe.set_defaults(fn=cmd_nm_finish_execution)

    sv = nmsub.add_parser(
        "serve", help="serve the authenticated six-operation HTTP interface"
    )
    sv.add_argument("--host", default="127.0.0.1")
    sv.add_argument("--port", type=int, default=8080)
    sv.set_defaults(fn=cmd_nm_serve)

    rm = nmsub.add_parser(
        "run-manifest",
        help="execute one accepted runner manifest and always emit a result",
    )
    rm.add_argument("manifest", help="accepted runner manifest JSON")
    rm.add_argument(
        "--bundle",
        default=None,
        help="verified compile-once build bundle (required for real boots)",
    )
    rm.add_argument(
        "--allow-development-bundle",
        action="store_true",
        help="allow a prebuilt development bundle for local proof only",
    )
    rm.add_argument(
        "--build-dir",
        default="build",
        help="charmOS build directory (default: build)",
    )
    rm.add_argument(
        "--out-dir",
        default=None,
        help="identity, copied manifest, boot evidence, and result directory",
    )
    rm.set_defaults(fn=cmd_nm_run_manifest)

    rp = nmsub.add_parser(
        "replay",
        help="re-execute a manifest, prior runner result, or result directory",
    )
    rp.add_argument("source", help="manifest, runner_result.json, or result directory")
    rp.add_argument(
        "--bundle",
        default=None,
        help="verified compile-once build bundle (required for real boots)",
    )
    rp.add_argument(
        "--allow-development-bundle",
        action="store_true",
        help="allow a prebuilt development bundle for local proof only",
    )
    rp.add_argument(
        "--build-dir",
        default="build",
        help="charmOS build directory (default: build)",
    )
    rp.add_argument(
        "--out-dir",
        default=None,
        help="new result directory (default: replay-results/<manifest>)",
    )
    rp.set_defaults(fn=cmd_nm_replay)

    return ap


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
