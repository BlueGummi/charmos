import json
import subprocess
from datetime import UTC, datetime
from pathlib import Path

import pytest

from charm.nightmare import build_bundle as B
from charm.nightmare import contracts

COMMIT = "56bc1dfdd14a0b3e03ff1d166e7af98bec8cbb6f"
IMAGE = "ghcr.io/axvonx/charmos-x86-env@sha256:" + "2" * 64
CONFIGURATION = {
    "compiler": "gcc",
    "type": "Debug",
    "cmake_definitions": ["TEST_NIGHTMARE_SMOKE=ON"],
    "smp": {"sockets": 1, "cores": 2, "threads": 1},
    "memory_mib": 512,
}


def request() -> B.BuildRequest:
    digest = contracts.sha256_json(
        {
            "source": {"repository": "axvonx/charmos", "commit": COMMIT},
            "runner_image": IMAGE,
            "configuration": CONFIGURATION,
        }
    )
    return B.BuildRequest(
        bundle_id=f"build_{digest[:20]}",
        request_sha256=digest,
        source_repository="axvonx/charmos",
        source_commit=COMMIT,
        runner_image=IMAGE,
        configuration=CONFIGURATION,
    )


def seed_inputs(root: Path, build_dir: Path) -> None:
    (build_dir / "kernel").mkdir(parents=True)
    (build_dir / "kernel" / "kernel").write_bytes(b"kernel")
    (build_dir / "d.img").write_bytes(b"disk")
    (root / "kernel").mkdir(parents=True)
    (root / "kernel" / "limine.conf").write_text(
        "/charmOS\n    path: boot():/boot/kernel\n"
    )
    (root / "limine").mkdir()
    for name in (
        "limine",
        "limine-bios.sys",
        "limine-bios-cd.bin",
        "limine-uefi-cd.bin",
        "BOOTX64.EFI",
        "BOOTIA32.EFI",
    ):
        path = root / "limine" / name
        path.write_bytes(name.encode())
    (root / "limine" / "limine").chmod(0o755)


def create(tmp_path: Path) -> B.VerifiedBundle:
    root = tmp_path / "repo"
    build_dir = tmp_path / "build"
    seed_inputs(root, build_dir)
    return B.create_bundle(
        request(),
        build_dir=build_dir,
        out_dir=tmp_path / "bundle",
        repo_root=root,
        now=datetime(2026, 8, 29, tzinfo=UTC),
        compile_kernel=False,
    )


def test_bundle_creation_is_content_addressed_and_complete(tmp_path: Path) -> None:
    bundle = create(tmp_path)
    other_root = tmp_path / "other"
    seed_inputs(other_root / "repo", other_root / "build")
    second = B.create_bundle(
        request(),
        build_dir=other_root / "build",
        out_dir=other_root / "bundle",
        repo_root=other_root / "repo",
        now=datetime(2027, 1, 1, tzinfo=UTC),
        compile_kernel=False,
    )

    assert bundle.bundle_id == request().bundle_id
    assert bundle.request_sha256 == request().request_sha256
    assert bundle.sha256 == bundle.document["sha256"]
    assert B.receipt(bundle) == {
        "bundle_id": request().bundle_id,
        "request_sha256": request().request_sha256,
        "sha256": bundle.sha256,
    }
    assert len(bundle.document["artifacts"]) == 10
    assert B.verify_bundle(bundle.root, expected=request()) == bundle
    assert second.sha256 == bundle.sha256


def test_compile_once_builds_kernel_and_pristine_disk_only(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "repo"
    (root / "scripts").mkdir(parents=True)
    calls: list[list[str]] = []

    def fake_run(
        command: list[str], **_kwargs: object
    ) -> subprocess.CompletedProcess[str]:
        calls.append(command)
        if command[:2] == ["git", "rev-parse"]:
            return subprocess.CompletedProcess(command, 0, COMMIT + "\n", "")
        if command[:2] == ["git", "status"]:
            return subprocess.CompletedProcess(command, 0, "", "")
        return subprocess.CompletedProcess(command, 0, "compiled once\n", "")

    monkeypatch.setattr(B.subprocess, "run", fake_run)
    log = B.compile_request(request(), build_dir=tmp_path / "build", repo_root=root)

    build_calls = [command for command in calls if command[0].endswith("build.sh")]
    assert len(build_calls) == 1
    assert "kernel" in build_calls[0]
    assert "pristine-disk" in build_calls[0]
    assert "tests" not in build_calls[0]
    assert log == "compiled once\n"


def test_bundle_rejects_artifact_and_metadata_tampering(tmp_path: Path) -> None:
    bundle = create(tmp_path)
    (bundle.root / "artifacts" / "kernel").write_bytes(b"tampered")
    with pytest.raises(B.BundleError, match="artifact digest mismatch"):
        B.verify_bundle(bundle.root)

    bundle = create(tmp_path / "second")
    metadata_path = bundle.root / B.METADATA_NAME
    metadata = json.loads(metadata_path.read_text())
    metadata["source"]["commit"] = "f" * 40
    metadata_path.write_text(json.dumps(metadata))
    with pytest.raises(B.BundleError, match="metadata digest mismatch"):
        B.verify_bundle(bundle.root)


def test_bundle_rejects_unexpected_files_and_expected_identity_mismatch(
    tmp_path: Path,
) -> None:
    bundle = create(tmp_path)
    (bundle.root / "not-declared").write_text("no")
    with pytest.raises(B.BundleError, match="file set mismatch"):
        B.verify_bundle(bundle.root)

    (bundle.root / "not-declared").unlink()
    with pytest.raises(B.BundleError, match="content digest mismatch"):
        B.verify_bundle(bundle.root, expected_sha256="f" * 64)

    mismatches = {
        "source": B.BuildRequest(
            bundle_id=request().bundle_id,
            request_sha256=request().request_sha256,
            source_repository="someone/else",
            source_commit=COMMIT,
            runner_image=IMAGE,
            configuration=CONFIGURATION,
        ),
        "runner_image": B.BuildRequest(
            bundle_id=request().bundle_id,
            request_sha256=request().request_sha256,
            source_repository=request().source_repository,
            source_commit=COMMIT,
            runner_image="ghcr.io/axvonx/other@sha256:" + "3" * 64,
            configuration=CONFIGURATION,
        ),
        "configuration": B.BuildRequest(
            bundle_id=request().bundle_id,
            request_sha256=request().request_sha256,
            source_repository=request().source_repository,
            source_commit=COMMIT,
            runner_image=IMAGE,
            configuration={**CONFIGURATION, "memory_mib": 1024},
        ),
    }
    for field, wrong in mismatches.items():
        with pytest.raises(B.BundleError, match=rf"{field} does not match"):
            B.verify_bundle(bundle.root, expected=wrong)


def test_bundle_rejects_incomplete_artifact_declarations(tmp_path: Path) -> None:
    bundle = create(tmp_path)
    metadata_path = bundle.root / B.METADATA_NAME
    metadata = json.loads(metadata_path.read_text())
    metadata["artifacts"] = [
        artifact
        for artifact in metadata["artifacts"]
        if artifact["name"] != "pristine_disk"
    ]
    metadata["sha256"] = B._bundle_digest(metadata)
    metadata_path.write_text(json.dumps(metadata))

    with pytest.raises(B.BundleError, match="artifact set mismatch"):
        B.verify_bundle(bundle.root)


def test_request_is_derived_from_accepted_plan_group() -> None:
    expected = request()
    plan = {
        "kind": "accepted",
        "plan": {
            "source": {
                "repository": expected.source_repository,
                "commit": expected.source_commit,
            },
            "runnerImage": expected.runner_image,
            "buildGroups": [
                {
                    "id": expected.bundle_id,
                    "requestSha256": expected.request_sha256,
                    "configuration": expected.configuration,
                }
            ],
        },
    }
    assert B.request_from_plan(plan, expected.bundle_id) == expected
    with pytest.raises(B.BundleError, match="require an accepted plan"):
        B.request_from_plan(plan["plan"], expected.bundle_id)


def test_qemu_command_uses_explicit_console_then_machine_serials(
    tmp_path: Path,
) -> None:
    bundle = create(tmp_path)
    command = B.qemu_command(
        bundle,
        iso_path=tmp_path / "boot.iso",
        disk_path=tmp_path / "disk.img",
        machine_log=tmp_path / "machine.nd.log",
        trace_log=tmp_path / "trace.log",
        qmp_socket=tmp_path / "qmp.sock",
    )

    assert "-nographic" not in command
    assert command[command.index("-display") + 1] == "none"
    serials = [
        command[index + 1] for index, value in enumerate(command) if value == "-serial"
    ]
    assert serials == ["stdio", f"file:{tmp_path / 'machine.nd.log'}"]


def test_transport_measurement_covers_upload_and_download(tmp_path: Path) -> None:
    bundle = create(tmp_path)
    measurement = B.measure_transport(bundle, tmp_path / "measurement")

    assert measurement["transport"] == "local_tar_gzip_proxy"
    assert measurement["bundle_size_bytes"] > 0
    assert measurement["archive_size_bytes"] > 0
    assert measurement["pack_ms"] >= 0
    assert measurement["upload_ms"] >= 0
    assert measurement["download_ms"] >= 0
    assert measurement["unpack_ms"] >= 0
