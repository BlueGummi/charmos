"""Identity-first execution of an accepted Nightmare runner manifest."""

import json
import shutil
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from . import campaign as campaign_model
from . import contracts as contract_model

IDENTITY_NAME = "identity.json"
MANIFEST_NAME = "manifest.json"
RESULT_NAME = "runner_result.json"


@dataclass(frozen=True)
class RunnerExecution:
    out_dir: Path
    result_path: Path
    document: dict[str, Any]
    exit_code: int


def _instant(now: Callable[[], datetime]) -> str:
    value = now()
    if value.tzinfo is None:
        value = value.replace(tzinfo=UTC)
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _write_json(path: Path, document: Any) -> None:
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def _copy_manifest(source: Path, destination: Path) -> None:
    if source.resolve() == destination.resolve():
        return
    shutil.copy2(source, destination)


def _identity(
    *,
    manifest_sha256: str,
    manifest: contract_model.RunnerManifest | None = None,
) -> dict[str, Any]:
    document: dict[str, Any] = {
        "schema_version": 1,
        "manifest_id": None,
        "manifest_sha256": manifest_sha256,
        "source": None,
        "suite": None,
        "build": None,
    }
    if manifest is not None:
        document.update(
            {
                "manifest_id": manifest.manifest_id,
                "source": {
                    "repository": manifest.source.repository,
                    "commit": manifest.source.commit,
                },
                "suite": {
                    "id": manifest.suite.id,
                    "sha256": manifest.suite.sha256,
                },
                "build": {
                    "bundle_id": manifest.build.bundle_id,
                    "sha256": manifest.build.sha256,
                    "runner_image": manifest.build.runner_image,
                },
            }
        )
    return document


def _replay_argv() -> list[str]:
    return [
        "python3",
        "-m",
        "charm",
        "nightmare",
        "replay",
        MANIFEST_NAME,
    ]


def _result_document(
    *,
    manifest_sha256: str,
    started_at: str,
    ended_at: str,
    lifecycle: contract_model.ExecutionLifecycle,
    discovery: contract_model.DiscoveryKind,
    finding_count: int,
    health: contract_model.ExecutionHealth,
    code: str,
    message: str,
    manifest: contract_model.RunnerManifest | None = None,
    campaign: campaign_model.CampaignResult | None = None,
) -> dict[str, Any]:
    campaign_doc = (
        json.loads(campaign_model.render_json(campaign))
        if campaign is not None
        else None
    )
    return {
        "schema_version": 1,
        "result_id": f"result_{manifest_sha256[:24]}",
        "manifest_id": manifest.manifest_id if manifest is not None else None,
        "manifest_sha256": manifest_sha256,
        "lifecycle": lifecycle.value,
        "discovery": {
            "kind": discovery.value,
            "finding_count": finding_count,
        },
        "execution": {
            "health": health.value,
            "code": code,
            "message": message[:2048],
        },
        "started_at": started_at,
        "ended_at": ended_at,
        "campaign": campaign_doc,
        "artifacts": {
            "identity": IDENTITY_NAME,
            "manifest": MANIFEST_NAME,
            "campaign_json": ("campaign_report.json" if campaign is not None else None),
            "campaign_markdown": (
                "campaign_summary.md" if campaign is not None else None
            ),
        },
        "replay": {"argv": _replay_argv()},
    }


def execute_manifest(
    manifest_path: Path,
    *,
    build_dir: Path,
    out_dir: Path,
    bundle_path: Path | None = None,
    allow_development_bundle: bool = False,
    boot_runner: campaign_model.BootRunner | None = None,
    clock: campaign_model.CampaignClock | None = None,
    now: Callable[[], datetime] = lambda: datetime.now(UTC),
) -> RunnerExecution:
    """Execute one manifest and always leave an identity and runner result."""
    manifest_path = manifest_path.resolve()
    build_dir = build_dir.resolve()
    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    started_at = _instant(now)
    try:
        manifest_sha256 = contract_model.sha256_file(manifest_path)
    except OSError:
        # There is no artifact location to copy when the input itself is absent.
        manifest_sha256 = contract_model.sha256_json(
            {"missing_manifest": str(manifest_path)}
        )

    identity_path = out_dir / IDENTITY_NAME
    copied_manifest_path = out_dir / MANIFEST_NAME
    result_path = out_dir / RESULT_NAME
    _write_json(
        identity_path,
        _identity(
            manifest_sha256=manifest_sha256,
        ),
    )

    manifest: contract_model.RunnerManifest | None = None
    try:
        _copy_manifest(manifest_path, copied_manifest_path)
        manifest = contract_model.load_manifest(copied_manifest_path)
    except (OSError, contract_model.ContractError) as error:
        document = _result_document(
            manifest_sha256=manifest_sha256,
            started_at=started_at,
            ended_at=_instant(now),
            lifecycle=contract_model.ExecutionLifecycle.FAILED,
            discovery=contract_model.DiscoveryKind.NONE,
            finding_count=0,
            health=contract_model.ExecutionHealth.INFRASTRUCTURE,
            code="invalid_manifest",
            message=str(error),
        )
        _write_json(result_path, document)
        return RunnerExecution(out_dir, result_path, document, 2)

    # This replaces the provisional sidecar before any boot-capable code runs.
    _write_json(
        identity_path,
        _identity(
            manifest_sha256=manifest_sha256,
            manifest=manifest,
        ),
    )

    campaign_manifest = campaign_model.CampaignManifest(
        suite=manifest.suite.model,
        runner_index=manifest.campaign.runner_index,
        total_runners=manifest.campaign.total_runners,
        base_seed=manifest.campaign.base_seed,
        campaign_id=manifest.campaign.campaign_id,
        budget_ms=manifest.campaign.soft_budget_ms,
        build_dir=build_dir,
        out_dir=out_dir,
        gate_first=manifest.campaign.gate_first,
        dry_run=manifest.campaign.dry_run,
    )

    if bundle_path is not None:
        from ..paths import repo_root
        from . import build_bundle as bundle_model

        configuration = manifest.suite.resolved["build"]
        request_sha256 = contract_model.sha256_json(
            {
                "source": {
                    "repository": manifest.source.repository,
                    "commit": manifest.source.commit,
                },
                "runner_image": manifest.build.runner_image,
                "configuration": configuration,
            }
        )
        expected = bundle_model.BuildRequest(
            bundle_id=manifest.build.bundle_id,
            request_sha256=request_sha256,
            source_repository=manifest.source.repository,
            source_commit=manifest.source.commit,
            runner_image=manifest.build.runner_image,
            configuration=configuration,
        )
        try:
            verified = bundle_model.verify_bundle(
                bundle_path,
                expected=expected,
                expected_sha256=manifest.build.sha256,
            )
            if not verified.production_ready and not allow_development_bundle:
                raise bundle_model.BundleError(
                    "prebuilt development bundle is not accepted for production execution"
                )
        except bundle_model.BundleError as error:
            document = _result_document(
                manifest_sha256=manifest_sha256,
                started_at=started_at,
                ended_at=_instant(now),
                lifecycle=contract_model.ExecutionLifecycle.FAILED,
                discovery=contract_model.DiscoveryKind.NONE,
                finding_count=0,
                health=contract_model.ExecutionHealth.INFRASTRUCTURE,
                code="invalid_build_bundle",
                message=str(error),
                manifest=manifest,
            )
            _write_json(result_path, document)
            return RunnerExecution(out_dir, result_path, document, 2)
        if boot_runner is None and not manifest.campaign.dry_run:
            boot_runner = campaign_model.BundleBootRunner(verified, repo_root())
    elif boot_runner is None and not manifest.campaign.dry_run:
        document = _result_document(
            manifest_sha256=manifest_sha256,
            started_at=started_at,
            ended_at=_instant(now),
            lifecycle=contract_model.ExecutionLifecycle.FAILED,
            discovery=contract_model.DiscoveryKind.NONE,
            finding_count=0,
            health=contract_model.ExecutionHealth.INFRASTRUCTURE,
            code="missing_build_bundle",
            message="non-dry-run manifests require a verified build bundle",
            manifest=manifest,
        )
        _write_json(result_path, document)
        return RunnerExecution(out_dir, result_path, document, 2)

    try:
        campaign = campaign_model.execute(
            campaign_manifest,
            boot_runner=boot_runner,
            clock=clock,
        )
        campaign_model.write_reports(campaign, out_dir)
        health = campaign.execution_health
        healthy = health == contract_model.ExecutionHealth.HEALTHY
        document = _result_document(
            manifest_sha256=manifest_sha256,
            started_at=started_at,
            ended_at=_instant(now),
            lifecycle=(
                contract_model.ExecutionLifecycle.COMPLETED
                if healthy
                else contract_model.ExecutionLifecycle.FAILED
            ),
            discovery=campaign.discovery_kind,
            finding_count=sum(item.occurrences for item in campaign.findings),
            health=health,
            code=campaign.status,
            message=f"campaign {campaign.status}",
            manifest=manifest,
            campaign=campaign,
        )
        exit_code = 0 if healthy else 1
    except KeyboardInterrupt:
        document = _result_document(
            manifest_sha256=manifest_sha256,
            started_at=started_at,
            ended_at=_instant(now),
            lifecycle=contract_model.ExecutionLifecycle.CANCELLED,
            discovery=contract_model.DiscoveryKind.NONE,
            finding_count=0,
            health=contract_model.ExecutionHealth.PARTIAL,
            code="cancelled",
            message="execution cancelled",
            manifest=manifest,
        )
        exit_code = 130
    except Exception as error:  # runner adapters are an infrastructure boundary
        document = _result_document(
            manifest_sha256=manifest_sha256,
            started_at=started_at,
            ended_at=_instant(now),
            lifecycle=contract_model.ExecutionLifecycle.FAILED,
            discovery=contract_model.DiscoveryKind.NONE,
            finding_count=0,
            health=contract_model.ExecutionHealth.INFRASTRUCTURE,
            code="runner_exception",
            message=f"{type(error).__name__}: {error}",
            manifest=manifest,
        )
        exit_code = 1

    _write_json(result_path, document)
    return RunnerExecution(out_dir, result_path, document, exit_code)


def resolve_replay_manifest(source: Path) -> Path:
    """Resolve a manifest, runner-result file, or prior result directory."""
    source = source.resolve()
    if source.is_dir():
        candidate = source / MANIFEST_NAME
    elif source.name == RESULT_NAME:
        try:
            document = json.loads(source.read_text(encoding="utf-8"))
            artifact = document["artifacts"]["manifest"]
            if not isinstance(artifact, str):
                raise TypeError("manifest artifact must be a string")
        except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
            raise contract_model.ContractError(
                str(source),
                [
                    contract_model.Diagnostic(
                        "runner_result.artifacts.manifest", str(error)
                    )
                ],
            ) from None
        candidate = source.parent / artifact
    else:
        candidate = source

    if not candidate.is_file():
        raise contract_model.ContractError(
            str(source),
            [contract_model.Diagnostic("replay.manifest", f"not found: {candidate}")],
        )
    return candidate
