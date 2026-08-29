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
