from pathlib import Path

from charm import workflow_policy as WP


def test_every_workflow_follows_policy() -> None:
    assert WP.check() == []


def test_check_covers_more_than_the_execution_workflows() -> None:
    names = {path.name for path in WP.all_paths()}
    assert set(WP.EXECUTION_WORKFLOWS) < names


def test_publisher_can_write_but_build_cannot(tmp_path: Path) -> None:
    publisher = tmp_path / "update.yml"
    build = tmp_path / "build.yml"
    text = "permissions:\n  contents: write\n"
    publisher.write_text(text)
    build.write_text(text)

    violations = WP.check((publisher, build))
    assert [(v.path.name, v.rule) for v in violations] == [
        ("build.yml", "repository_write"),
    ]


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
        (5, "container_without_packages_read"),
    ]


def test_git_rules_are_scoped_to_execution_workflows() -> None:
    text = """
permissions:
  contents: write
  packages: read
run: git push origin main
"""
    assert [v.rule for v in WP.check_text(Path("w.yml"), text)] == [
        "repository_write",
        "git_mutation",
    ]
    assert WP.check_text(Path("w.yml"), text, execution=False) == []


def test_registry_rules_still_apply_to_a_non_execution_workflow() -> None:
    text = """
permissions:
  contents: write
jobs:
  bump:
    container:
      image: ghcr.io/example/kernel@sha256:{d}
""".format(d="a" * 64)
    violations = WP.check_text(Path("w.yml"), text, execution=False)
    assert [v.rule for v in violations] == ["container_without_packages_read"]


def test_packages_read_satisfies_the_container_rule() -> None:
    text = """
permissions:
  contents: write
  packages: read
jobs:
  bump:
    container:
      image: ghcr.io/example/kernel@sha256:{d}
""".format(d="a" * 64)
    assert WP.check_text(Path("w.yml"), text, execution=False) == []


def test_a_container_without_any_permissions_block_is_left_alone() -> None:
    text = """
jobs:
  bump:
    container:
      image: ghcr.io/example/kernel@sha256:{d}
""".format(d="a" * 64)
    assert WP.check_text(Path("w.yml"), text) == []


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
  plan:
    steps:
      - run: python3 -m charm nightmare plan x
"""
    violations = WP.check_text(Path("workflow.yml"), text)
    assert [(v.line, v.rule) for v in violations] == [(5, "charm_not_importable")]


def test_charm_invocation_through_uv_is_clean() -> None:
    text = """
jobs:
  plan:
    steps:
      - run: uv run --locked charm nightmare plan x
"""
    assert WP.check_text(Path("workflow.yml"), text) == []


def test_a_workflow_that_never_runs_charm_needs_no_pythonpath() -> None:
    text = "jobs:\n  build:\n    steps:\n      - run: ./scripts/build.sh iso\n"
    assert WP.check_text(Path("workflow.yml"), text) == []
