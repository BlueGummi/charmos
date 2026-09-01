from pathlib import Path
from subprocess import CompletedProcess

import pytest

from charm.nightmare import build_bundle


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
