"""Compile once, verify deeply, and repack without compiling the kernel."""

import json
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath
from typing import Any

from ..paths import repo_root as default_repo_root
from . import contracts

METADATA_NAME = "bundle.json"
ARTIFACT_DIR = "artifacts"
_ID = re.compile(r"^[a-z][a-z0-9_:-]{0,127}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_COMMIT = re.compile(r"^[0-9a-f]{40}$")
_REPOSITORY = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
_IMAGE = re.compile(r"^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$")
_DEFINITION = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=[^\s]*$")


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


@dataclass(frozen=True)
class RepackMeasurement:
    iso_path: Path
    repack_ms: float
    iso_size_bytes: int


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


def _instant(now: datetime) -> str:
    return now.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _configuration_args(configuration: dict[str, Any]) -> list[str]:
    smp = configuration["smp"]
    return [
        *(f"-D{definition}" for definition in configuration["cmake_definitions"]),
        f"-DQEMU_SMP_TOPO=sockets={smp['sockets']},cores={smp['cores']},threads={smp['threads']}",
        f"-DQEMU_MEM_SIZE={configuration['memory_mib']}M",
        "-DQEMU_NUMA=OFF"
        if smp["sockets"] * smp["cores"] * smp["threads"] < 4
        else "-DQEMU_NUMA=ON",
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


def _validate_configuration(configuration: Any) -> None:
    if not isinstance(configuration, dict) or set(configuration) != {
        "compiler",
        "type",
        "cmake_definitions",
        "smp",
        "memory_mib",
    }:
        raise BundleError("bundle configuration has an invalid shape")
    if configuration["compiler"] not in ("gcc", "clang"):
        raise BundleError("bundle compiler is not allowlisted")
    if configuration["type"] not in (
        "Debug",
        "Release",
        "RelWithDebInfo",
        "MinSizeRel",
    ):
        raise BundleError("bundle build type is not allowlisted")
    definitions = configuration["cmake_definitions"]
    if (
        not isinstance(definitions, list)
        or not all(
            isinstance(item, str) and _DEFINITION.fullmatch(item)
            for item in definitions
        )
        or len(definitions) != len(set(definitions))
    ):
        raise BundleError("bundle CMake definitions are invalid")
    smp = configuration["smp"]
    if (
        not isinstance(smp, dict)
        or set(smp) != {"sockets", "cores", "threads"}
        or not all(type(smp[field]) is int and smp[field] >= 1 for field in smp)
    ):
        raise BundleError("bundle SMP topology is invalid")
    if type(configuration["memory_mib"]) is not int or configuration["memory_mib"] < 64:
        raise BundleError("bundle memory_mib is invalid")


def _validate_metadata(document: dict[str, Any]) -> None:
    required = {
        "schema_version",
        "bundle_id",
        "request_sha256",
        "sha256",
        "source",
        "runner_image",
        "configuration",
        "provenance",
        "artifacts",
        "created_at",
    }
    if set(document) != required or document.get("schema_version") != 1:
        raise BundleError("bundle metadata has an invalid top-level shape")
    if not isinstance(document["bundle_id"], str) or not _ID.fullmatch(
        document["bundle_id"]
    ):
        raise BundleError("bundle_id is invalid")
    if not isinstance(document["request_sha256"], str) or not _SHA256.fullmatch(
        document["request_sha256"]
    ):
        raise BundleError("bundle request_sha256 is invalid")
    if not isinstance(document["sha256"], str) or not _SHA256.fullmatch(
        document["sha256"]
    ):
        raise BundleError("bundle sha256 is invalid")
    source = document["source"]
    if (
        not isinstance(source, dict)
        or set(source) != {"repository", "commit"}
        or not isinstance(source["repository"], str)
        or not _REPOSITORY.fullmatch(source["repository"])
        or not isinstance(source["commit"], str)
        or not _COMMIT.fullmatch(source["commit"])
    ):
        raise BundleError("bundle source identity is invalid")
    if not isinstance(document["runner_image"], str) or not _IMAGE.fullmatch(
        document["runner_image"]
    ):
        raise BundleError("bundle runner image is not immutable")
    _validate_configuration(document["configuration"])
    if document["provenance"] not in (
        {"mode": "compiled_clean"},
        {"mode": "prebuilt_development"},
    ):
        raise BundleError("bundle provenance is invalid")
    if not isinstance(document["created_at"], str):
        raise BundleError("bundle created_at is invalid")
    try:
        created_at = datetime.fromisoformat(
            document["created_at"].replace("Z", "+00:00")
        )
    except ValueError:
        raise BundleError("bundle created_at is invalid") from None
    if created_at.tzinfo is None:
        raise BundleError("bundle created_at must include a UTC offset")
    request_digest = contracts.sha256_json(
        {
            "source": source,
            "runner_image": document["runner_image"],
            "configuration": document["configuration"],
        }
    )
    if document["request_sha256"] != request_digest:
        raise BundleError("bundle request digest does not match its content")


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
        "created_at": _instant(now or datetime.now(UTC)),
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
    _validate_metadata(document)
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
    artifacts = document.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise BundleError("bundle must declare at least one artifact")
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != {
            "name",
            "path",
            "sha256",
        }:
            raise BundleError("malformed artifact record")
        try:
            name = artifact["name"]
            path = _safe_artifact_path(root, artifact["path"])
            expected_digest = artifact["sha256"]
        except (KeyError, TypeError) as error:
            raise BundleError(f"malformed artifact record: {error}") from None
        if not isinstance(name, str) or not _ID.fullmatch(name):
            raise BundleError("invalid artifact name")
        if not isinstance(expected_digest, str) or not _SHA256.fullmatch(
            expected_digest
        ):
            raise BundleError(f"invalid artifact digest: {name}")
        if name in names:
            raise BundleError(f"duplicate artifact name: {name}")
        if name not in _EXPECTED_ARTIFACTS:
            raise BundleError(f"unexpected bundle artifact name: {name}")
        if artifact["path"] != _EXPECTED_ARTIFACTS[name]:
            raise BundleError(f"bundle artifact path is invalid for {name}")
        if path in declared:
            raise BundleError(f"duplicate artifact path: {artifact['path']}")
        if not path.is_file():
            raise BundleError(f"bundle artifact is missing: {artifact['path']}")
        if contracts.sha256_file(path) != expected_digest:
            raise BundleError(f"artifact digest mismatch: {artifact['path']}")
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


def repack(
    bundle: VerifiedBundle,
    *,
    cmdline: Path,
    out_dir: Path,
    repo_root: Path | None = None,
) -> RepackMeasurement:
    root = (repo_root or default_repo_root()).resolve()
    verify_bundle(bundle.root, expected_sha256=bundle.sha256)
    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    iso_path = out_dir / "charmos-x86_64.iso"
    started = time.perf_counter()
    completed = subprocess.run(
        [
            "cmake",
            f"-DBUNDLE_DIR={bundle.root}",
            f"-DCMDLINE={cmdline.resolve()}",
            f"-DOUTPUT_ISO={iso_path}",
            f"-DWORK_DIR={out_dir / 'repack-work'}",
            "-P",
            str(root / "cmake" / "repack_nightmare_bundle.cmake"),
        ],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000
    if completed.returncode != 0:
        raise BundleError(
            f"bundle repack failed with exit code {completed.returncode}: "
            f"{completed.stdout}{completed.stderr}"
        )
    return RepackMeasurement(iso_path, elapsed_ms, iso_path.stat().st_size)


def qemu_command(
    bundle: VerifiedBundle,
    *,
    iso_path: Path,
    disk_path: Path,
    machine_log: Path,
    trace_log: Path,
    qmp_socket: Path,
) -> list[str]:
    """Build the fixed runner command from verified bundle configuration."""
    configuration = bundle.document["configuration"]
    smp = configuration["smp"]
    total_cpus = smp["sockets"] * smp["cores"] * smp["threads"]
    memory_mib = configuration["memory_mib"]
    command = [
        "qemu-system-x86_64",
        "-cdrom",
        str(iso_path),
        "-boot",
        "d",
        "-m",
        f"{memory_mib}M",
        "-smp",
        f"sockets={smp['sockets']},cores={smp['cores']},threads={smp['threads']}",
        "-M",
        "q35",
        "-qmp",
        f"unix:{qmp_socket},server,nowait",
        "-monitor",
        "none",
        "-device",
        "intel-iommu,intremap=on",
        "-device",
        "qemu-xhci,id=xhci",
        "-device",
        "usb-kbd,bus=xhci.0,port=1,id=usbkbd",
        "-device",
        "usb-mouse,bus=xhci.0,port=2,id=usbmouse",
        "-drive",
        f"id=nvme0,file={disk_path},format=raw,if=none",
        "-device",
        "nvme,serial=boom,drive=nvme0",
        "-d",
        "trace:*xhci*",
        "-trace",
        f"file={trace_log}",
        "-device",
        "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-display",
        "none",
        "-serial",
        "stdio",
        "-serial",
        f"file:{machine_log}",
    ]
    if total_cpus >= 4:
        per_node_mib = memory_mib // 4
        if per_node_mib * 4 != memory_mib:
            raise BundleError("NUMA bundle memory must divide evenly across four nodes")
        for index in range(4):
            command.extend(
                [
                    "-object",
                    f"memory-backend-ram,size={per_node_mib}M,id=mem{index}",
                ]
            )
        cpus_per_node = total_cpus // 4
        if cpus_per_node * 4 != total_cpus:
            raise BundleError("NUMA bundle CPUs must divide evenly across four nodes")
        for index in range(4):
            first = index * cpus_per_node
            last = first + cpus_per_node - 1
            command.extend(
                [
                    "-numa",
                    f"node,cpus={first}-{last},nodeid={index},memdev=mem{index}",
                ]
            )
    return command


def measure_transport(bundle: VerifiedBundle, out_dir: Path) -> dict[str, Any]:
    """Measure local archive pack/unpack as the pre-Actions transport baseline."""
    out_dir.mkdir(parents=True, exist_ok=True)
    archive = out_dir / f"{bundle.bundle_id}.tar.gz"
    pack_started = time.perf_counter()
    with tarfile.open(archive, "w:gz") as handle:
        handle.add(bundle.root, arcname=bundle.bundle_id)
    pack_ms = (time.perf_counter() - pack_started) * 1000
    with tempfile.TemporaryDirectory(prefix="charmos-bundle-measure-") as temp:
        transport_root = Path(temp)
        uploaded = transport_root / "artifact-store.tar.gz"
        upload_started = time.perf_counter()
        shutil.copyfile(archive, uploaded)
        upload_ms = (time.perf_counter() - upload_started) * 1000
        downloaded = transport_root / "runner-download.tar.gz"
        download_started = time.perf_counter()
        shutil.copyfile(uploaded, downloaded)
        download_ms = (time.perf_counter() - download_started) * 1000
        if contracts.sha256_file(downloaded) != contracts.sha256_file(archive):
            raise BundleError("transported bundle archive digest mismatch")
        unpack_started = time.perf_counter()
        with tarfile.open(downloaded, "r:gz") as handle:
            handle.extractall(transport_root / "unpacked", filter="data")
        unpack_ms = (time.perf_counter() - unpack_started) * 1000
    return {
        "transport": "local_tar_gzip_proxy",
        "bundle_size_bytes": sum(
            path.stat().st_size for path in bundle.root.rglob("*") if path.is_file()
        ),
        "archive_size_bytes": archive.stat().st_size,
        "pack_ms": round(pack_ms, 3),
        "upload_ms": round(upload_ms, 3),
        "download_ms": round(download_ms, 3),
        "unpack_ms": round(unpack_ms, 3),
    }


def receipt(bundle: VerifiedBundle) -> dict[str, str]:
    return {
        "bundle_id": bundle.bundle_id,
        "request_sha256": bundle.request_sha256,
        "sha256": bundle.sha256,
    }
