import gzip
import os
import re
import shutil
import sys
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, TextIO

from . import record as R

ENV_KEYS = (
    "SHARD",
    "COMPILER",
    "SCENARIO",
    "SEED",
    "SHA",
    "CONFIG",
    "EXIT_CODE",
    "DURATION_S",
)

ENV_LINE_RE = re.compile(r"^([A-Z_][A-Z0-9_]*)=(.*)$")
SAFE_SHARD_RE = re.compile(r"^[A-Za-z0-9._-]+$")


@dataclass
class Problem:
    level: str  # "error" | "warning"
    text: str


@dataclass
class Shard:
    env_path: Path
    base: Path
    env: dict[str, str] = field(default_factory=dict)

    @property
    def name(self) -> str:
        return self.env.get("SHARD") or self.base.name

    @property
    def console_log(self) -> Path:
        return self.base.with_name(f"{self.base.name}.log")

    @property
    def machine_log(self) -> Path:
        return self.base.with_name(f"{self.base.name}.nd.log")


def parse_env(text: str) -> dict[str, str]:
    """Read ``KEY=VALUE`` lines. Unknown keys are ignored, not executed."""
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = ENV_LINE_RE.match(line)
        if not m:
            continue
        key, value = m.group(1), m.group(2).strip()
        # Artifacts are written unquoted, but tolerate a shell-ish quote pair.
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        if key in ENV_KEYS:
            out[key] = value
    return out


def discover(root: Path) -> list[Shard]:
    """Every ``*.env`` under root, deepest-stable order."""
    shards = []
    for env_path in sorted(root.rglob("*.env")):
        if not env_path.is_file():
            continue
        base = env_path.with_suffix("")
        shard = Shard(env_path=env_path, base=base)
        shard.env = parse_env(env_path.read_text(encoding="utf-8", errors="replace"))
        shards.append(shard)
    return shards


def gunzip_in_place(path: Path) -> bool:
    """Expand ``path.gz`` to ``path``. True when something was expanded."""
    packed = path.with_name(f"{path.name}.gz")
    if not packed.is_file():
        return False
    with gzip.open(packed, "rb") as src, path.open("wb") as dst:
        shutil.copyfileobj(src, dst)
    packed.unlink()
    return True


def meta_of(shard: Shard) -> R.RunMeta:
    env = shard.env

    def num(key: str, cast: Callable[[str], Any]) -> Any | None:
        raw = env.get(key, "")
        if raw == "":
            return None
        try:
            return cast(raw)
        except ValueError:
            return None

    return R.RunMeta(
        shard=shard.name,
        scenario=env.get("SCENARIO") or None,
        seed=env.get("SEED") or None,
        sha=env.get("SHA") or None,
        compiler=env.get("COMPILER") or None,
        config=env.get("CONFIG") or None,
        exit_code=num("EXIT_CODE", int),
        duration_s=num("DURATION_S", float),
    )


def ingest(root: Path, out_dir: Path) -> tuple[list[Path], list[Problem]]:
    """Parse every shard under root into out_dir. Returns (written, problems)."""
    problems: list[Problem] = []
    written: list[Path] = []
    claimed: dict[str, Path] = {}

    shards = discover(root)
    if not shards:
        problems.append(Problem("error", f"no .env files found in {root}"))
        return written, problems

    out_dir.mkdir(parents=True, exist_ok=True)

    for shard in shards:
        name = shard.name
        if not SAFE_SHARD_RE.match(name):
            problems.append(
                Problem(
                    "error",
                    f"{shard.env_path}: SHARD={name!r} is not a valid filename; skipping",
                )
            )
            continue

        gunzip_in_place(shard.console_log)
        gunzip_in_place(shard.machine_log)

        if not shard.console_log.is_file():
            problems.append(
                Problem(
                    "warning",
                    f"{name}: console log not found beside {shard.env_path}; skipping",
                )
            )
            continue

        if not shard.machine_log.is_file():
            problems.append(Problem("error", f"{name} produced no NDJSON channel"))
            shard.machine_log.write_text("", encoding="utf-8")

        out_path = out_dir / f"{name}.ndjson"
        if name in claimed:
            problems.append(
                Problem(
                    "error",
                    f"{name}: duplicate shard name found in {claimed[name]} and {shard.env_path}",
                )
            )
            continue
        claimed[name] = shard.env_path

        with shard.machine_log.open(encoding="utf-8", errors="replace") as fh:
            result = R.parse_ndjson(fh)

        for err in result.stats.schema_errors:
            problems.append(Problem("warning", f"{name} schema drift: {err}"))
            result.records.append({"type": "schema_error", "detail": err})

        if not result.stats.schema_seen:
            problems.append(
                Problem(
                    "warning",
                    f"{name}: no schema records found (boot with ndjson.schema=true)",
                )
            )

        run = R.build_run_record(
            result, meta_of(shard), shard.console_log.stat().st_size
        )
        out_path.write_text(R.render([run, *result.records]), encoding="utf-8")
        written.append(out_path)

    return written, problems


def report_problems(problems: list[Problem], out: TextIO = sys.stderr) -> None:
    """Print with GitHub annotations when CI is watching, plainly otherwise."""
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    for p in problems:
        prefix = f"::{p.level}::" if annotate else f"{p.level}: "
        print(f"{prefix}{p.text}", file=out)
