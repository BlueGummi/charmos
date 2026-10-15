from charm.paths import repo_root

WORKFLOWS = repo_root() / ".github" / "workflows"


def test_materialize_finds_receipts_in_any_artifact_layout() -> None:
    text = (WORKFLOWS / "nightmare-orchestrator.yml").read_text()
    assert "builds/*/receipt.json" not in text
    assert "find builds -name receipt.json" in text
