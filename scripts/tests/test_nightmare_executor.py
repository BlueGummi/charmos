import json
from pathlib import Path

from charm.cli import build_parser, main
from charm.nightmare import campaign as NC
from charm.nightmare import executor as NE
from charm.nightmare import suite as NS
from charm.paths import nightmare_dir

FIXTURE = nightmare_dir() / "fixtures" / "contracts" / "valid" / "runner_manifest.json"


def _manifest(tmp_path: Path, *, dry_run: bool) -> Path:
    document = json.loads(FIXTURE.read_text(encoding="utf-8"))
    document["campaign"]["dry_run"] = dry_run
    path = tmp_path / "input-manifest.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


class IdentityCheckingRunner:
    def __init__(self, finding: bool = False) -> None:
        self.finding = finding
        self.calls = 0

    def run_boot(
        self,
        manifest: NC.CampaignManifest,
        task: NS.Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> NC.BootResult:
        identity = json.loads((out_dir / NE.IDENTITY_NAME).read_text())
        assert identity["manifest_id"] == "manifest_contract_smoke"
        assert identity["source"]["commit"]
        self.calls += 1
        findings = (
            [
                NC.FindingRecord(
                    sig="fixture_sig",
                    tier="confident",
                    kind="invariant",
                    site="fixture:1",
                    msg="scripted finding",
                    boot_index=boot_index,
                )
            ]
            if self.finding
            else []
        )
        return NC.BootResult(
            boot_index=boot_index,
            task_name=task.name,
            cmdline=cmdline,
            seed=None,
            duration_ms=1,
            exit_code=0,
            status=(
                NC.BootStatus.FINDING.value if self.finding else NC.BootStatus.OK.value
            ),
            reason="scripted",
            progress=1,
            findings=findings,
        )


class RaisingRunner:
    def run_boot(
        self,
        manifest: NC.CampaignManifest,
        task: NS.Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> NC.BootResult:
        raise RuntimeError("fixture runner failure")


def test_dry_run_writes_identity_manifest_reports_and_result(tmp_path: Path) -> None:
    source = _manifest(tmp_path, dry_run=True)
    out_dir = tmp_path / "result"

    execution = NE.execute_manifest(
        source,
        build_dir=tmp_path / "build",
        out_dir=out_dir,
    )

    assert execution.exit_code == 0
    assert execution.document["lifecycle"] == "completed"
    assert execution.document["execution"]["health"] == "healthy"
    assert execution.document["discovery"]["kind"] == "none"
    assert (out_dir / NE.IDENTITY_NAME).is_file()
    assert (out_dir / NE.MANIFEST_NAME).is_file()
    assert (out_dir / "campaign_report.json").is_file()
    assert (out_dir / "campaign_summary.md").is_file()
    assert execution.result_path.is_file()


def test_identity_exists_before_scripted_boot_and_finding_is_healthy(
    tmp_path: Path,
) -> None:
    source = _manifest(tmp_path, dry_run=False)
    runner = IdentityCheckingRunner(finding=True)

    execution = NE.execute_manifest(
        source,
        build_dir=tmp_path / "build",
        out_dir=tmp_path / "result",
        boot_runner=runner,
    )

    assert runner.calls == 1
    assert execution.exit_code == 0
    assert execution.document["execution"]["health"] == "healthy"
    assert execution.document["discovery"] == {
        "kind": "finding",
        "finding_count": 1,
    }


def test_invalid_manifest_still_writes_identity_and_result(tmp_path: Path) -> None:
    source = tmp_path / "invalid.json"
    source.write_text("{}", encoding="utf-8")
    out_dir = tmp_path / "result"

    execution = NE.execute_manifest(
        source,
        build_dir=tmp_path / "build",
        out_dir=out_dir,
    )

    assert execution.exit_code == 2
    assert execution.document["manifest_id"] is None
    assert execution.document["lifecycle"] == "failed"
    assert execution.document["execution"]["code"] == "invalid_manifest"
    assert (out_dir / NE.IDENTITY_NAME).is_file()
    assert execution.result_path.is_file()


def test_runner_exception_becomes_infrastructure_result(tmp_path: Path) -> None:
    execution = NE.execute_manifest(
        _manifest(tmp_path, dry_run=False),
        build_dir=tmp_path / "build",
        out_dir=tmp_path / "result",
        boot_runner=RaisingRunner(),
    )

    assert execution.exit_code == 1
    assert execution.document["execution"] == {
        "health": "infrastructure",
        "code": "runner_exception",
        "message": "RuntimeError: fixture runner failure",
    }
    assert execution.result_path.is_file()


def test_real_manifest_requires_a_verified_build_bundle(tmp_path: Path) -> None:
    execution = NE.execute_manifest(
        _manifest(tmp_path, dry_run=False),
        build_dir=tmp_path / "build",
        out_dir=tmp_path / "result",
    )

    assert execution.exit_code == 2
    assert execution.document["execution"] == {
        "health": "infrastructure",
        "code": "missing_build_bundle",
        "message": "non-dry-run manifests require a verified build bundle",
    }


def test_replay_resolves_manifest_result_and_directory(tmp_path: Path) -> None:
    source = _manifest(tmp_path, dry_run=True)
    execution = NE.execute_manifest(
        source,
        build_dir=tmp_path / "build",
        out_dir=tmp_path / "result",
    )
    copied = execution.out_dir / NE.MANIFEST_NAME

    assert NE.resolve_replay_manifest(copied) == copied
    assert NE.resolve_replay_manifest(execution.result_path) == copied
    assert NE.resolve_replay_manifest(execution.out_dir) == copied


def test_manifest_commands_are_exposed_by_the_cli(tmp_path: Path) -> None:
    parser = build_parser()
    run_args = parser.parse_args(
        ["nightmare", "run-manifest", str(FIXTURE), "--out-dir", str(tmp_path)]
    )
    replay_args = parser.parse_args(["nightmare", "replay", str(tmp_path)])

    assert run_args.fn.__name__ == "cmd_nm_run_manifest"
    assert replay_args.fn.__name__ == "cmd_nm_replay"


def test_run_manifest_cli_reports_a_contract_failure(tmp_path: Path) -> None:
    source = tmp_path / "invalid.json"
    source.write_text("{}", encoding="utf-8")

    exit_code = main(
        [
            "nightmare",
            "run-manifest",
            str(source),
            "--out-dir",
            str(tmp_path / "result"),
        ]
    )

    assert exit_code == 2
    assert (tmp_path / "result" / NE.RESULT_NAME).is_file()
