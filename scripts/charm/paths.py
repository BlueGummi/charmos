import os
from functools import lru_cache
from pathlib import Path

MARKERS = ("include/ndjson.h", "kernel/CMakeLists.txt")


@lru_cache(maxsize=1)
def repo_root() -> Path:
    override = os.environ.get("CHARMOS_ROOT")
    if override:
        return Path(override).resolve()

    for start in (Path(__file__).resolve(), Path.cwd().resolve() / "_"):
        for parent in start.parents:
            if all((parent / m).is_file() for m in MARKERS):
                return parent

    raise SystemExit(
        "charm: cannot locate the charmos checkout from "
        f"{Path(__file__).resolve()} or {Path.cwd()}; set CHARMOS_ROOT"
    )


def nightmare_dir() -> Path:
    return repo_root() / "nightmare"
