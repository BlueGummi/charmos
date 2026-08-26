"""nightmare tasks into kernel command lines"""

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import grammar as g
from .suite import Build, Suite, Task

ROOT = "nightmare"


class CodecError(ValueError):
    pass


@dataclass(frozen=True)
class BootRequest:
    """Parameters that vary per boot"""

    boot_index: int = 0
    campaign_id: str | None = None
    seed: int | None = None


def seed_for(
    base_seed: int, *, runner: int, boot_index: int, boots_per_runner: int
) -> int:
    """Partition the seed space across runners and boots."""
    if runner < 0 or boot_index < 0 or boots_per_runner < 1:
        raise CodecError("seed partition indices must be non-negative")
    offset = runner * boots_per_runner + boot_index
    return (base_seed + offset) & 0xFFFFFFFFFFFFFFFF


def render(task: Task, request: BootRequest | None = None) -> str:
    """One task plus one boot's identity becomes one command line."""
    req = request or BootRequest()
    n, b = task.nightmare, task.boot
    tokens: list[str] = []

    def add(key: str, value: str) -> None:
        tokens.append(g.pair(key, value))

    add(ROOT, task.name)
    add(f"{ROOT}.intensity", g.fx(n.intensity))
    add(f"{ROOT}.seed_mode", n.seed_mode)

    if n.wants_seed:
        if req.seed is None:
            raise CodecError(
                f"task {task.name!r} with seed_mode={n.seed_mode!r} requires a seed"
            )
        add(f"{ROOT}.seed", g.hex_u64(req.seed))
    elif req.seed is not None:
        raise CodecError(
            f"task {task.name!r} is seed_mode='seedless' but a seed was supplied"
        )

    add(f"{ROOT}.duration_ms", g.duration_ms(b.duration_ms))
    add(f"{ROOT}.drain_grace_ms", g.duration_ms(b.drain_grace_ms))
    add(f"{ROOT}.stat_interval_ms", g.duration_ms(b.stat_interval_ms))
    add(f"{ROOT}.on_stall", b.on_stall)
    add(f"{ROOT}.boot_index", g.uint(req.boot_index))

    if req.campaign_id is not None:
        if not g.OPAQUE_RE.match(req.campaign_id):
            raise CodecError(
                f"invalid campaign_id {req.campaign_id!r}; allowed characters: [A-Za-z0-9._:-]"
            )
        add(f"{ROOT}.campaign_id", req.campaign_id)

    if n.perturb:
        add(f"{ROOT}.perturb", g.name_list(n.perturb))
        for svc in sorted(n.perturb_opts):
            for knob in sorted(n.perturb_opts[svc]):
                value = n.perturb_opts[svc][knob]
                add(f"{ROOT}.perturb.{svc}.{knob}", _knob(value, knob))

    for opt in sorted(n.opts):
        add(f"{ROOT}.{task.name}.{opt}", _knob(n.opts[opt], opt))

    return g.join(tokens)


def _knob(value: Any, name: str) -> str:
    from .suite import render_knob

    try:
        return render_knob(value, name)
    except g.GrammarError as e:
        raise CodecError(f"{name}: {e}") from None


def render_gate(_task: Task | None = None) -> str:
    """command line for the gate boot"""
    return ""


def write(path: Path, cmdline: str) -> Path:
    """the artifact gen_limine_conf.cmake reads via CMDLINE=<path>"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(cmdline + "\n", encoding="utf-8")
    return path


def build_args(suite: Suite) -> list[str]:
    """The cmake -D flags"""
    b: Build = suite.build
    args = [f"-D{d}" for d in b.cmake_definitions]
    args.append(f"-DQEMU_SMP_TOPO={b.smp.topo()}")
    args.append(f"-DQEMU_MEM_SIZE={_mem_size(b.memory_mib)}")
    return args


def _mem_size(mib: int) -> str:
    if mib % 1024 == 0:
        return f"{mib // 1024}G"
    return f"{mib}M"


def build_command(suite: Suite, target: str = "iso") -> list[str]:
    """The full `build.sh` invocation for the artifact"""
    b = suite.build
    return [
        "./scripts/build.sh",
        "-t",
        b.type,
        "--compiler",
        b.compiler,
        target,
        "--",
        *build_args(suite),
    ]
