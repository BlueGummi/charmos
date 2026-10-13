import os
import zipfile
from dataclasses import replace
from pathlib import Path
from subprocess import CompletedProcess

import pytest

from charm.nightmare import build_bundle, contracts


def request(commit: str) -> build_bundle.BuildRequest:
    return build_bundle.BuildRequest(
        bundle_id="build_test",
        request_sha256="0" * 64,
        source_repository="axvonx/charmos",
        source_commit=commit,
        runner_image=f"ghcr.io/axvonx/charmos@sha256:{'0' * 64}",
        configuration={
            "compiler": "gcc",
            "type": "Debug",
            "cmake_definitions": [],
            "smp": {"sockets": 1, "cores": 1, "threads": 1},
            "memory_mib": 512,
        },
    )


def test_compile_request_builds_limine_installer_after_kernel(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    commit = "a" * 40
    calls: list[list[str]] = []

    def run(command: list[str], **_kwargs: object) -> CompletedProcess[str]:
        calls.append(command)
        if command == ["git", "rev-parse", "HEAD"]:
            return CompletedProcess(command, 0, f"{commit}\n", "")
        if command == ["git", "status", "--porcelain", "--untracked-files=no"]:
            return CompletedProcess(command, 0, "", "")
        return CompletedProcess(command, 0, "built\n", "")

    monkeypatch.setattr(build_bundle.subprocess, "run", run)

    log = build_bundle.compile_request(
        request(commit), build_dir=tmp_path / "build", repo_root=tmp_path
    )

    assert calls[-2][0].endswith("scripts/build.sh")
    assert calls[-1] == ["make", "-C", str(tmp_path / "limine")]
    assert log == "built\nbuilt\n"


def test_compile_request_reports_limine_build_failure(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    commit = "b" * 40

    def run(command: list[str], **_kwargs: object) -> CompletedProcess[str]:
        if command == ["git", "rev-parse", "HEAD"]:
            return CompletedProcess(command, 0, f"{commit}\n", "")
        if command == ["git", "status", "--porcelain", "--untracked-files=no"]:
            return CompletedProcess(command, 0, "", "")
        if command[:2] == ["make", "-C"]:
            return CompletedProcess(command, 2, "", "compiler error\n")
        return CompletedProcess(command, 0, "kernel built\n", "")

    monkeypatch.setattr(build_bundle.subprocess, "run", run)

    with pytest.raises(build_bundle.BundleError, match="limine build failed"):
        build_bundle.compile_request(
            request(commit), build_dir=tmp_path / "build", repo_root=tmp_path
        )


def verifiable_request(commit: str) -> build_bundle.BuildRequest:
    base = request(commit)
    return replace(
        base,
        request_sha256=contracts.sha256_json(
            {
                "source": {
                    "repository": base.source_repository,
                    "commit": base.source_commit,
                },
                "runner_image": base.runner_image,
                "configuration": base.configuration,
            }
        ),
    )


def prebuilt_bundle(tmp_path: Path) -> build_bundle.VerifiedBundle:
    root = tmp_path / "repo"
    build_dir = tmp_path / "build"
    for name, (location, relative) in build_bundle._SOURCES.items():
        source = (build_dir if location == "build" else root) / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(f"{name}\n", encoding="utf-8")
    (root / "limine" / "limine").chmod(0o755)

    return build_bundle.create_bundle(
        verifiable_request("c" * 40),
        build_dir=build_dir,
        out_dir=tmp_path / "bundle",
        repo_root=root,
        compile_kernel=False,
    )


def transport(
    bundle: build_bundle.VerifiedBundle, destination: Path
) -> build_bundle.VerifiedBundle:
    archive = destination.with_suffix(".zip")
    with zipfile.ZipFile(archive, "w") as packed:
        for path in sorted(bundle.root.rglob("*")):
            if path.is_file():
                packed.write(path, path.relative_to(bundle.root))
    with zipfile.ZipFile(archive) as packed:
        packed.extractall(destination)
    return build_bundle.verify_bundle(destination)


def test_artifact_zip_round_trip_drops_the_installer_exec_bit(tmp_path: Path) -> None:
    built = prebuilt_bundle(tmp_path)
    assert os.access(built.root / build_bundle.ARTIFACT_DIR / "limine", os.X_OK)

    received = transport(built, tmp_path / "download")

    assert not os.access(received.root / build_bundle.ARTIFACT_DIR / "limine", os.X_OK)


def test_repack_restores_the_installer_exec_bit_before_use(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    received = transport(prebuilt_bundle(tmp_path), tmp_path / "download")
    installer = received.root / build_bundle.ARTIFACT_DIR / "limine"
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    executable_when_called: list[bool] = []

    def run(command: list[str], **_kwargs: object) -> CompletedProcess[str]:
        executable_when_called.append(os.access(installer, os.X_OK))
        (out_dir.resolve() / "charmos-x86_64.iso").write_text("iso", encoding="utf-8")
        return CompletedProcess(command, 0, "", "")

    monkeypatch.setattr(build_bundle.subprocess, "run", run)
    cmdline = tmp_path / "cmdline"
    cmdline.write_text("nightmare=harness_smoke\n", encoding="utf-8")

    iso_path = build_bundle.repack(received, cmdline=cmdline, out_dir=out_dir)

    assert executable_when_called == [True, True]
    assert iso_path == out_dir.resolve() / "charmos-x86_64.iso"
