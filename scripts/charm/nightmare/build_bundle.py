import json
import shutil
import subprocess
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath
from typing import Any, Literal

from pydantic import AwareDatetime, Field, ValidationError

from ..paths import repo_root as default_repo_root
from . import contracts
from . import suite as suite_model

METADATA_NAME = "bundle.json"
ARTIFACT_DIR = "artifacts"


class BundleError(ValueError):
    pass


@dataclass(frozen=True)
class BuildRequest:
    bundle_id: str
    request_sha256: str
    source_repository: str
    source_commit: str
    runner_image: str
    configuration: dict[str, Any]


@dataclass(frozen=True)
class VerifiedBundle:
    root: Path
    document: dict[str, Any]

    @property
    def bundle_id(self) -> str:
        return self.document["bundle_id"]

    @property
    def sha256(self) -> str:
        return self.document["sha256"]

    @property
    def request_sha256(self) -> str:
        return self.document["request_sha256"]

    @property
    def pristine_disk(self) -> Path:
        return self.root / ARTIFACT_DIR / "d.img"

    @property
    def production_ready(self) -> bool:
        return self.document.get("provenance", {}).get("mode") == "compiled_clean"


def request_from_plan(plan: dict[str, Any], group_id: str) -> BuildRequest:
    if plan.get("kind") != "accepted" or not isinstance(plan.get("plan"), dict):
        raise BundleError("build bundles require an accepted plan result")
    plan = plan["plan"]
    try:
        group = next(item for item in plan["buildGroups"] if item["id"] == group_id)
        source = plan["source"]
        request = BuildRequest(
            bundle_id=group["id"],
            request_sha256=group["requestSha256"],
            source_repository=source["repository"],
            source_commit=source["commit"],
            runner_image=plan["runnerImage"],
            configuration=group["configuration"],
        )
    except (KeyError, StopIteration, TypeError) as error:
        raise BundleError(
            f"accepted plan has no build group {group_id!r}: {error}"
        ) from None
    actual_digest = contracts.sha256_json(
        {
            "source": {
                "repository": request.source_repository,
                "commit": request.source_commit,
            },
            "runner_image": request.runner_image,
            "configuration": request.configuration,
        }
    )
    if request.request_sha256 != actual_digest:
        raise BundleError("accepted build request digest does not match its content")
    return request


_SOURCES = {
    "kernel": ("build", "kernel/kernel"),
    "pristine_disk": ("build", "d.img"),
    "limine_conf": ("repo", "kernel/limine.conf"),
    "limine": ("repo", "limine/limine"),
    "limine_bios_sys": ("repo", "limine/limine-bios.sys"),
    "limine_bios_cd": ("repo", "limine/limine-bios-cd.bin"),
    "limine_uefi_cd": ("repo", "limine/limine-uefi-cd.bin"),
    "bootx64_efi": ("repo", "limine/BOOTX64.EFI"),
    "bootia32_efi": ("repo", "limine/BOOTIA32.EFI"),
}

_DESTINATIONS = {
    "kernel": "kernel",
    "pristine_disk": "d.img",
    "limine_conf": "limine.conf",
    "limine": "limine",
    "limine_bios_sys": "limine-bios.sys",
    "limine_bios_cd": "limine-bios-cd.bin",
    "limine_uefi_cd": "limine-uefi-cd.bin",
    "bootx64_efi": "BOOTX64.EFI",
    "bootia32_efi": "BOOTIA32.EFI",
}
_EXPECTED_ARTIFACTS = {
    **{
        name: f"{ARTIFACT_DIR}/{destination}"
        for name, destination in _DESTINATIONS.items()
    },
    "build_log": f"{ARTIFACT_DIR}/build.log",
}

EXECUTABLE_ARTIFACTS = ("limine",)


def _configuration_args(configuration: dict[str, Any]) -> list[str]:
    smp = configuration["smp"]
    return [
        *(f"-D{definition}" for definition in configuration["cmake_definitions"]),
        f"-DMACHINE_SMP=sockets={smp['sockets']},cores={smp['cores']},threads={smp['threads']}",
        f"-DMACHINE_MEMORY={configuration['memory_mib']}M",
    ]


def compile_request(
    request: BuildRequest,
    *,
    build_dir: Path,
    repo_root: Path | None = None,
) -> str:
    root = (repo_root or default_repo_root()).resolve()
    try:
        actual_commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise BundleError(f"cannot establish source identity: {error}") from None
    if actual_commit != request.source_commit:
        raise BundleError(
            f"source commit mismatch: expected {request.source_commit}, got {actual_commit}"
        )
    if dirty:
        raise BundleError(
            "refusing to compile an identity-bearing bundle from a dirty tree"
        )

    command = [
        str(root / "scripts" / "build.sh"),
        "-B",
        str(build_dir.resolve()),
        "-t",
        request.configuration["type"],
        "--compiler",
        request.configuration["compiler"],
        "kernel",
        "pristine-disk",
        "--",
        *_configuration_args(request.configuration),
        f"-DCHARMOS_COMMIT={request.source_commit}",
    ]
    completed = subprocess.run(
        command,
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    log = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise BundleError(
            f"build failed with exit code {completed.returncode}\n{log[-4000:]}"
        )

    limine = subprocess.run(
        ["make", "-C", str(root / "limine")],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    log += limine.stdout + limine.stderr
    if limine.returncode != 0:
        raise BundleError(
            f"limine build failed with exit code {limine.returncode}\n{log[-4000:]}"
        )
    return log


def _bundle_digest(document: dict[str, Any]) -> str:
    payload = {
        key: value
        for key, value in document.items()
        if key not in ("sha256", "created_at")
    }
    return contracts.sha256_json(payload)


class Artifact(contracts.Contract):
    name: str
    path: str
    sha256: contracts.Sha256


class Provenance(contracts.Contract):
    mode: Literal["compiled_clean", "prebuilt_development"]


class BundleMetadata(contracts.Contract):
    schema_version: Literal[1]
    bundle_id: contracts.Id
    request_sha256: contracts.Sha256
    sha256: contracts.Sha256
    source: contracts.SourceIdentity
    runner_image: contracts.Image
    configuration: suite_model.Build
    provenance: Provenance
    artifacts: list[Artifact] = Field(min_length=1)
    created_at: AwareDatetime = Field(strict=False)


def _validate_metadata(document: dict[str, Any]) -> BundleMetadata:
    try:
        metadata = BundleMetadata.model_validate(document)
    except ValidationError as error:
        diagnostics = suite_model.diagnostics_of(error, "bundle")
        raise BundleError(
            "bundle metadata is invalid: " + "; ".join(map(str, diagnostics))
        ) from None
    request_digest = contracts.sha256_json(
        {
            "source": document["source"],
            "runner_image": document["runner_image"],
            "configuration": document["configuration"],
        }
    )
    if metadata.request_sha256 != request_digest:
        raise BundleError("bundle request digest does not match its content")
    return metadata


def create_bundle(
    request: BuildRequest,
    *,
    build_dir: Path,
    out_dir: Path,
    repo_root: Path | None = None,
    now: datetime | None = None,
    compile_kernel: bool = True,
    build_log: str | None = None,
) -> VerifiedBundle:
    root = (repo_root or default_repo_root()).resolve()
    build_dir = build_dir.resolve()
    out_dir = out_dir.resolve()
    if out_dir.exists() and any(out_dir.iterdir()):
        raise BundleError(f"bundle output directory is not empty: {out_dir}")
    artifact_dir = out_dir / ARTIFACT_DIR
    artifact_dir.mkdir(parents=True, exist_ok=True)

    if compile_kernel:
        build_log = compile_request(request, build_dir=build_dir, repo_root=root)
    log_path = artifact_dir / "build.log"
    log_path.write_text(build_log or "prebuilt bundle fixture\n", encoding="utf-8")

    artifacts: list[dict[str, str]] = []
    for name, (location, relative) in _SOURCES.items():
        source = (build_dir if location == "build" else root) / relative
        if not source.is_file():
            raise BundleError(f"required bundle input is missing: {source}")
        destination = artifact_dir / _DESTINATIONS[name]
        shutil.copy2(source, destination)
        artifacts.append(
            {
                "name": name,
                "path": str(destination.relative_to(out_dir)),
                "sha256": contracts.sha256_file(destination),
            }
        )
    artifacts.append(
        {
            "name": "build_log",
            "path": str(log_path.relative_to(out_dir)),
            "sha256": contracts.sha256_file(log_path),
        }
    )
    document = {
        "schema_version": 1,
        "bundle_id": request.bundle_id,
        "request_sha256": request.request_sha256,
        "sha256": "0" * 64,
        "source": {
            "repository": request.source_repository,
            "commit": request.source_commit,
        },
        "runner_image": request.runner_image,
        "configuration": request.configuration,
        "provenance": {
            "mode": "compiled_clean" if compile_kernel else "prebuilt_development"
        },
        "artifacts": sorted(artifacts, key=lambda item: item["name"]),
        "created_at": contracts.instant(now or datetime.now(UTC)),
    }
    document["sha256"] = _bundle_digest(document)
    (out_dir / METADATA_NAME).write_text(
        json.dumps(document, indent=2) + "\n", encoding="utf-8"
    )
    return verify_bundle(out_dir, expected=request)


def _safe_artifact_path(root: Path, value: str) -> Path:
    pure = PurePosixPath(value)
    if pure.is_absolute() or ".." in pure.parts:
        raise BundleError(f"unsafe artifact path: {value!r}")
    path = root.joinpath(*pure.parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise BundleError(f"artifact escapes bundle root: {value!r}")
    return path


def verify_bundle(
    root: Path,
    *,
    expected: BuildRequest | None = None,
    expected_sha256: str | None = None,
) -> VerifiedBundle:
    root = root.resolve()
    try:
        document = json.loads((root / METADATA_NAME).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BundleError(f"cannot read bundle metadata: {error}") from None
    if not isinstance(document, dict):
        raise BundleError("bundle metadata must be an object")
    digest = _bundle_digest(document)
    if document.get("sha256") != digest:
        raise BundleError("bundle metadata digest mismatch")
    metadata = _validate_metadata(document)
    if expected_sha256 is not None and digest != expected_sha256:
        raise BundleError(
            f"bundle content digest mismatch: expected {expected_sha256}, got {digest}"
        )
    if expected is not None:
        comparisons = {
            "bundle_id": expected.bundle_id,
            "request_sha256": expected.request_sha256,
            "source": {
                "repository": expected.source_repository,
                "commit": expected.source_commit,
            },
            "runner_image": expected.runner_image,
            "configuration": expected.configuration,
        }
        for field, value in comparisons.items():
            if document.get(field) != value:
                raise BundleError(
                    f"bundle {field} does not match the accepted build request"
                )

    declared: set[Path] = set()
    names: set[str] = set()
    for artifact in metadata.artifacts:
        name = artifact.name
        path = _safe_artifact_path(root, artifact.path)
        if name in names:
            raise BundleError(f"duplicate artifact name: {name}")
        if name not in _EXPECTED_ARTIFACTS:
            raise BundleError(f"unexpected bundle artifact name: {name}")
        if artifact.path != _EXPECTED_ARTIFACTS[name]:
            raise BundleError(f"bundle artifact path is invalid for {name}")
        if path in declared:
            raise BundleError(f"duplicate artifact path: {artifact.path}")
        if not path.is_file():
            raise BundleError(f"bundle artifact is missing: {artifact.path}")
        if contracts.sha256_file(path) != artifact.sha256:
            raise BundleError(f"artifact digest mismatch: {artifact.path}")
        names.add(name)
        declared.add(path)
    if names != set(_EXPECTED_ARTIFACTS):
        raise BundleError(
            "bundle artifact set mismatch "
            f"(missing={sorted(set(_EXPECTED_ARTIFACTS) - names)}, "
            f"unexpected={sorted(names - set(_EXPECTED_ARTIFACTS))})"
        )
    actual = {path.resolve() for path in root.rglob("*") if path.is_file()}
    expected_files = declared | {(root / METADATA_NAME).resolve()}
    if actual != expected_files:
        unexpected = sorted(
            str(path.relative_to(root)) for path in actual - expected_files
        )
        missing = sorted(
            str(path.relative_to(root)) for path in expected_files - actual
        )
        raise BundleError(
            f"bundle file set mismatch (missing={missing}, unexpected={unexpected})"
        )
    return VerifiedBundle(root, document)


def restore_executable_bits(bundle: VerifiedBundle) -> None:
    """Give back the exec bits that transporting the bundle dropped."""
    for name in EXECUTABLE_ARTIFACTS:
        path = bundle.root / ARTIFACT_DIR / _DESTINATIONS[name]
        path.chmod(path.stat().st_mode | 0o111)


def repack_iso(
    bundle_dir: Path, cmdline: Path, output_iso: Path, work_dir: Path
) -> None:
    import mkiso

    artifacts = bundle_dir / ARTIFACT_DIR
    try:
        mkiso.build(
            artifacts / "kernel",
            artifacts / "limine.conf",
            artifacts,
            output_iso,
            mkiso.resolve_cmdline({"CMDLINE": str(cmdline)}),
            work_dir,
        )
    except mkiso.ConfError as error:
        raise BundleError(str(error)) from error


def repack(
    bundle: VerifiedBundle,
    *,
    cmdline: Path,
    out_dir: Path,
) -> Path:
    """The bundle's ISO rebuilt around one kernel command line"""
    verify_bundle(bundle.root, expected_sha256=bundle.sha256)
    restore_executable_bits(bundle)
    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    iso_path = out_dir / "charmos-x86_64.iso"
    repack_iso(
        bundle.root,
        cmdline=cmdline.resolve(),
        output_iso=iso_path,
        work_dir=out_dir / "repack-work",
    )
    return iso_path


def qemu_command(
    bundle: VerifiedBundle,
    *,
    iso_path: Path,
    disk_path: Path,
    machine_log: Path,
    trace_log: Path,
    qmp_socket: Path,
) -> list[str]:
    """Runner commnad"""
    from .. import machine as machine_model

    configuration = bundle.document["configuration"]
    smp = configuration["smp"]
    profile = machine_model.load(configuration.get("machine_profile", "default"))
    return machine_model.render(
        profile,
        "nightmare",
        iso=iso_path,
        disk=disk_path,
        qmp_socket=qmp_socket,
        machine_log=machine_log,
        trace_log=trace_log,
        memory_mib=configuration["memory_mib"],
        smp=machine_model.Smp(
            sockets=smp["sockets"], cores=smp["cores"], threads=smp["threads"]
        ),
    )


def receipt(bundle: VerifiedBundle) -> dict[str, str]:
    return {
        "bundle_id": bundle.bundle_id,
        "request_sha256": bundle.request_sha256,
        "sha256": bundle.sha256,
    }
