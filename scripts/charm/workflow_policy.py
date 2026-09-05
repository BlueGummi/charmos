"""Static law for workflows that build or execute CharmOS."""

import re
from dataclasses import dataclass
from pathlib import Path

from .paths import repo_root

PROTECTED_WORKFLOWS = (
    "build.yml",
    "nightmare-contract.yml",
    "nightmare-orchestrator.yml",
    "nightmare-waker.yml",
    "nightmare.yml",
    "test.yml",
    "tools.yml",
    "update.yml",
)

_RULES = (
    (
        "repository_write",
        re.compile(r"\bcontents\s*:\s*write\b"),
        "automation may not write repository contents",
    ),
    (
        "git_mutation",
        re.compile(r"\bgit\s+(?:commit|push)\b"),
        "automation may not commit or push",
    ),
    (
        "broad_registry_secret",
        re.compile(r"secrets\.(?:GHCR_KEY|PAT|GITHUB_PAT|PERSONAL_ACCESS_TOKEN)\b"),
        "use the job-scoped GITHUB_TOKEN instead of a broad registry secret",
    ),
)

_CHARM_INVOCATION = re.compile(r"\bpython3?\s+-m\s+charm\b")
_PYTHONPATH = re.compile(r"\bPYTHONPATH\b")

_RUNNER_IMAGE = re.compile(r"\bimage\s*:\s*(ghcr\.io/[^\s#]+)")
_IMMUTABLE_IMAGE = re.compile(
    r"^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$"
)


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    rule: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.message}"


def check_text(path: Path, text: str) -> list[Violation]:
    """Return every policy violation in one workflow."""
    violations: list[Violation] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        runner_image = _RUNNER_IMAGE.search(line)
        if runner_image is not None and not _IMMUTABLE_IMAGE.fullmatch(
            runner_image.group(1)
        ):
            violations.append(
                Violation(
                    path,
                    line_number,
                    "mutable_runner_image",
                    "runner images must use an immutable sha256 digest",
                )
            )
        for name, pattern, message in _RULES:
            if pattern.search(line):
                violations.append(Violation(path, line_number, name, message))

    violations.extend(_check_charm_importable(path, text))
    return violations


def _check_charm_importable(path: Path, text: str) -> list[Violation]:
    """
    PYTHONPATH must point at scripts/
    """
    if _PYTHONPATH.search(text):
        return []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        if _CHARM_INVOCATION.search(line):
            return [
                Violation(
                    path,
                    line_number,
                    "charm_not_importable",
                    "workflows that run `python -m charm` must set PYTHONPATH",
                )
            ]
    return []


def protected_paths(root: Path | None = None) -> tuple[Path, ...]:
    base = (root or repo_root()) / ".github" / "workflows"
    return tuple(base / name for name in PROTECTED_WORKFLOWS)


def check(paths: tuple[Path, ...] | None = None) -> list[Violation]:
    """Check the protected build/execution workflows."""
    violations: list[Violation] = []
    for path in paths or protected_paths():
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            violations.append(Violation(path, 0, "unreadable", str(error)))
            continue
        violations.extend(check_text(path, text))
    return violations
