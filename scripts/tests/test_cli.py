"""Every charm command group parses and runs its cheap, side-effect-free paths"""

from pathlib import Path

import pytest

from charm.cli import main
from charm.paths import repo_root

SUITE = str(repo_root() / "nightmare" / "suites" / "overnight_locks.toml")
IMAGE = "ghcr.io/axvonx/charmos-x86-env@sha256:" + "0" * 64
COMMIT = "56bc1dfdd14a0b3e03ff1d166e7af98bec8cbb6f"


@pytest.mark.parametrize(
    "argv",
    [
        ["ci", "protocol"],
        ["ci", "workflow-policy"],
        ["machine", "list"],
        ["nightmare", "show", SUITE],
        ["nightmare", "render", SUITE, "--seed", "0x1234"],
        ["nightmare", "build", SUITE],
        ["nightmare", "validate", SUITE],
    ],
)
def test_command_succeeds(argv: list[str]) -> None:
    assert main(argv) == 0


def test_dry_run_campaign_succeeds(tmp_path: Path) -> None:
    assert (
        main(["nightmare", "run", SUITE, "--dry-run", "--out-dir", str(tmp_path)]) == 0
    )


def test_planner_round_trip(tmp_path: Path) -> None:
    command, snapshot, plan = (tmp_path / n for n in ("c.json", "s.json", "p.json"))
    inputs = ["--command-out", str(command), "--snapshot-out", str(snapshot)]
    clock = ["--at", "2026-08-29T00:00:00Z"]
    repository = ["--suite", "harness_smoke", "--repository", "axvonx/charmos"]
    assert main(["nightmare", "repository-command", *repository, *clock, *inputs]) == 0
    accept = ["nightmare", "plan", str(command), str(snapshot), "--out", str(plan)]
    assert main([*accept, "--source-commit", COMMIT, "--runner-image", IMAGE]) == 0
    assert main(["nightmare", "matrix", "build", str(plan)]) == 0
    assert main([*accept, "--source-commit", "nope", "--runner-image", IMAGE]) == 2


def test_an_unreadable_input_is_an_error_not_a_traceback() -> None:
    assert main(["nightmare", "show", "/nonexistent.toml"]) == 2
