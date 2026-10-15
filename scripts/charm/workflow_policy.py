"""Static law for workflows that build or execute CharmOS."""

import re
from dataclasses import dataclass
from pathlib import Path

from .paths import repo_root

EXECUTION_WORKFLOWS = (
    "build.yml",
    "nightmare-orchestrator.yml",
    "nightmare.yml",
    "test.yml",
    "tools.yml",
)

_EXECUTION_RULES = (
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
)

# Applied to every workflow
_UNIVERSAL_RULES = (
    (
        "broad_registry_secret",
        re.compile(r"secrets\.(?:GHCR_KEY|PAT|GITHUB_PAT|PERSONAL_ACCESS_TOKEN)\b"),
        "use the job-scoped GITHUB_TOKEN instead of a broad registry secret",
    ),
)

_CHARM_INVOCATION = re.compile(r"\bpython3?\s+-m\s+charm\b")

_PERMISSIONS_BLOCK = re.compile(r"^\s*permissions\s*:\s*$", re.M)
_PACKAGES_SCOPE = re.compile(r"^\s+packages\s*:\s*(?:read|write)\s*$", re.M)

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


def check_text(path: Path, text: str, *, execution: bool = True) -> list[Violation]:
    rules = _UNIVERSAL_RULES + (_EXECUTION_RULES if execution else ())

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
        for name, pattern, message in rules:
            if pattern.search(line):
                violations.append(Violation(path, line_number, name, message))

    violations.extend(_check_charm_importable(path, text))
    violations.extend(_check_container_can_pull(path, text))
    return violations


def _check_container_can_pull(path: Path, text: str) -> list[Violation]:
    if not _PERMISSIONS_BLOCK.search(text):
        return []
    if _PACKAGES_SCOPE.search(text):
        return []

    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        if _RUNNER_IMAGE.search(line) is None:
            continue
        return [
            Violation(
                path,
                line_number,
                "container_without_packages_read",
                "a job running in a GHCR container needs `packages: read` "
                "once the workflow declares a permissions block",
            )
        ]
    return []


def _check_charm_importable(path: Path, text: str) -> list[Violation]:
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        if _CHARM_INVOCATION.search(line):
            return [
                Violation(
                    path,
                    line_number,
                    "charm_not_importable",
                    "run charm as `uv run --locked charm` so its locked dependencies are present",
                )
            ]
    return []


def workflows_dir(root: Path | None = None) -> Path:
    return (root or repo_root()) / ".github" / "workflows"


def all_paths(root: Path | None = None) -> tuple[Path, ...]:
    base = workflows_dir(root)
    return tuple(sorted(base.glob("*.yml")) + sorted(base.glob("*.yaml")))


def check(paths: tuple[Path, ...] | None = None) -> list[Violation]:
    violations: list[Violation] = []
    for path in paths or all_paths():
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            violations.append(Violation(path, 0, "unreadable", str(error)))
            continue
        violations.extend(
            check_text(path, text, execution=path.name in EXECUTION_WORKFLOWS)
        )
    return violations
