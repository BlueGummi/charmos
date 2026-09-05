from pathlib import Path

from charm import workflow_policy as WP


def test_protected_workflows_follow_policy() -> None:
    assert WP.check() == []


def test_every_rule_reports_its_line() -> None:
    text = """
permissions:
  contents: write
run: git push origin main
image: ghcr.io/example/kernel:latest
password: ${{ secrets.GHCR_KEY }}
"""
    violations = WP.check_text(Path("workflow.yml"), text)
    assert [(v.line, v.rule) for v in violations] == [
        (3, "repository_write"),
        (4, "git_mutation"),
        (5, "mutable_runner_image"),
        (6, "broad_registry_secret"),
    ]


def test_comments_do_not_trigger_policy() -> None:
    text = "# do not use image: ghcr.io/example/kernel:latest\n"
    assert WP.check_text(Path("workflow.yml"), text) == []


def test_every_ghcr_runner_tag_is_mutable_even_when_it_looks_versioned() -> None:
    text = "container:\n  image: ghcr.io/example/kernel:v2026.08\n"
    violations = WP.check_text(Path("workflow.yml"), text)
    assert [item.rule for item in violations] == ["mutable_runner_image"]


def test_charm_invocation_without_pythonpath_is_a_violation() -> None:
    text = """
jobs:
  wake:
    steps:
      - run: python3 -m charm nightmare wake-queue
"""
    violations = WP.check_text(Path("workflow.yml"), text)
    assert [(v.line, v.rule) for v in violations] == [(5, "charm_not_importable")]


def test_charm_invocation_with_pythonpath_is_clean() -> None:
    text = """
env:
  PYTHONPATH: scripts
jobs:
  wake:
    steps:
      - run: python3 -m charm nightmare wake-queue
"""
    assert WP.check_text(Path("workflow.yml"), text) == []


def test_a_workflow_that_never_runs_charm_needs_no_pythonpath() -> None:
    text = "jobs:\n  build:\n    steps:\n      - run: ./scripts/build.sh iso\n"
    assert WP.check_text(Path("workflow.yml"), text) == []
