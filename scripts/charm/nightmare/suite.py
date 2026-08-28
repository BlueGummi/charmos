"""Load, validate, and type check suite configs"""

import tomllib
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..paths import nightmare_dir
from . import grammar as g

SCHEMA_VERSION = 1
SCHEMA_PATH = nightmare_dir() / "schemas" / "suite-v1.schema.json"

SERVICES = (
    "migrator",
    "waker",
    "apc_spammer",
    "stutter",
    "alloc_pressure",
    "inject_armer",
)
PERTURB_KNOBS = {
    "migrator": frozenset({"interval_us"}),
    "waker": frozenset({"interval_us"}),
    "apc_spammer": frozenset({"interval_us"}),
    "stutter": frozenset({"period_ms", "gap_ms"}),
    "alloc_pressure": frozenset({"interval_us"}),
    "inject_armer": frozenset({"interval_us"}),
}

COMPILERS = ("gcc", "clang")
BUILD_TYPES = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
MODES = ("horizontal", "vertical")
SEED_MODES = ("split", "seedful", "seedless")
ON_STALL = ("report", "crash")

FLUSH_MARGIN_MS = 15000


class SuiteError(ValueError):
    """A suite that cannot be loaded or does not cohere"""

    def __init__(self, source: str, diagnostics: list["Diagnostic"]):
        self.source = source
        self.diagnostics = diagnostics
        body = "\n".join(f"  {d}" for d in diagnostics)
        super().__init__(f"{source}: {len(diagnostics)} problem(s)\n{body}")


@dataclass(frozen=True)
class Diagnostic:
    path: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}: {self.message}"


@dataclass(frozen=True)
class Smp:
    sockets: int = 1
    cores: int = 1
    threads: int = 1

    @property
    def total(self) -> int:
        return self.sockets * self.cores * self.threads

    def topo(self) -> str:
        return f"sockets={self.sockets},cores={self.cores},threads={self.threads}"


@dataclass(frozen=True)
class Build:
    compiler: str = "gcc"
    type: str = "RelWithDebInfo"
    cmake_definitions: tuple[str, ...] = ()
    smp: Smp = field(default_factory=Smp)
    memory_mib: int = 2048


@dataclass(frozen=True)
class Boot:
    duration_ms: int
    drain_grace_ms: int = 20000
    timeout_ms: int = 0  # 0 means "derive it"
    gate_first: bool = True
    max_boots: int = 500
    min_interval_ms: int = 0
    stat_interval_ms: int = 5000
    stall_threshold_ms: int = 3000
    on_stall: str = "report"

    @property
    def guest_hard_ms(self) -> int:
        return self.duration_ms + self.drain_grace_ms

    @property
    def host_timeout_ms(self) -> int:
        return self.timeout_ms or (self.guest_hard_ms + FLUSH_MARGIN_MS)


@dataclass(frozen=True)
class Nightmare:
    intensity: float = 0.5
    seed_mode: str = "split"
    perturb: tuple[str, ...] = ()
    perturb_opts: dict[str, dict[str, Any]] = field(default_factory=dict)
    opts: dict[str, Any] = field(default_factory=dict)

    @property
    def seeds_perturbers(self) -> bool:
        return self.seed_mode in ("split", "seedful")

    @property
    def seeds_subject(self) -> bool:
        return self.seed_mode == "seedful"

    @property
    def wants_seed(self) -> bool:
        """Whether a boot of this task carries a seed"""
        return self.seed_mode != "seedless"


@dataclass(frozen=True)
class Task:
    name: str
    boot: Boot
    nightmare: Nightmare = field(default_factory=Nightmare)
    mode: str = "horizontal"
    weight: float = 1.0
    priority: float = 1.0
    max_runners: int | None = None


@dataclass(frozen=True)
class SuiteMeta:
    name: str
    runners: int
    budget_hours: float
    overlap_ratio: float = 0.0

    @property
    def budget_ms(self) -> int:
        return int(self.budget_hours * 3600 * 1000)


@dataclass(frozen=True)
class Suite:
    meta: SuiteMeta
    build: Build
    tasks: tuple[Task, ...]
    source: str = "<memory>"

    def task(self, name: str) -> Task:
        for t in self.tasks:
            if t.name == name:
                return t
        known = ", ".join(t.name for t in self.tasks)
        raise KeyError(f"{self.source}: no task named {name!r} (have: {known})")


def _get(d: dict[str, Any], key: str, default: Any) -> Any:
    value = d.get(key)
    return default if value is None else value


def load(path: Path) -> Suite:
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise SuiteError(str(path), [Diagnostic("<toml>", str(e))]) from None
    except OSError as e:
        raise SuiteError(str(path), [Diagnostic("<file>", str(e))]) from None
    return from_dict(raw, source=str(path))


def from_dict(raw: dict, source: str = "<memory>") -> Suite:
    from_schema = schema_diagnostics(raw)

    build_errors: list[Diagnostic] = []
    suite = _build_suite(raw, build_errors)
    semantic = build_errors + (coherence_diagnostics(suite) if suite else [])

    diags = _merge(semantic, from_schema)
    if diags:
        raise SuiteError(source, diags)
    assert suite is not None
    return Suite(suite.meta, suite.build, suite.tasks, source=source)


def _merge(
    semantic: list[Diagnostic], from_schema: list[Diagnostic]
) -> list[Diagnostic]:
    """Merge semantic + schema diagnostics"""
    covered = {d.path for d in semantic}
    merged = semantic + [d for d in from_schema if d.path not in covered]
    return sorted(merged, key=lambda d: (d.path, d.message))


def _build_suite(raw: dict, diags: list[Diagnostic]) -> Suite | None:
    """Shape dict into the model"""
    if not isinstance(raw, dict):
        diags.append(Diagnostic("<root>", "expected a table"))
        return None

    before = len(diags)
    s = _get(raw, "suite", {})
    b = _get(raw, "build", {})
    ts = _get(raw, "tasks", [])

    if not isinstance(s, dict):
        diags.append(Diagnostic("suite", "expected a table"))
        s = {}
    else:
        for key in ("name", "runners", "budget_hours"):
            if key not in s:
                diags.append(Diagnostic(f"suite.{key}", "is required"))

    if not isinstance(b, dict):
        diags.append(Diagnostic("build", "expected a table"))
        b = {}

    if not isinstance(ts, list) or not ts:
        diags.append(Diagnostic("tasks", "expected at least one [[tasks]] entry"))
        ts = []

    if len(diags) != before:
        return None

    meta = SuiteMeta(
        name=s["name"],
        runners=s["runners"],
        budget_hours=float(s["budget_hours"]),
        overlap_ratio=float(_get(s, "overlap_ratio", 0.0)),
    )

    smp_raw = _get(b, "smp", {})
    smp = Smp(
        sockets=_get(smp_raw, "sockets", 1),
        cores=_get(smp_raw, "cores", 1),
        threads=_get(smp_raw, "threads", 1),
    )
    build = Build(
        compiler=_get(b, "compiler", "gcc"),
        type=_get(b, "type", "RelWithDebInfo"),
        cmake_definitions=tuple(_get(b, "cmake_definitions", [])),
        smp=smp,
        memory_mib=_get(b, "memory_mib", 2048),
    )

    tasks = []
    for i, t in enumerate(ts):
        if not isinstance(t, dict):
            diags.append(Diagnostic(f"tasks[{i}]", "expected a table"))
            continue
        if "name" not in t:
            diags.append(Diagnostic(f"tasks[{i}].name", "is required"))
            continue
        boot_raw = _get(t, "boot", {})
        if "duration_ms" not in boot_raw:
            diags.append(Diagnostic(f"tasks[{i}].boot.duration_ms", "is required"))
            continue
        nm_raw = _get(t, "nightmare", {})
        tasks.append(
            Task(
                name=t["name"],
                mode=_get(t, "mode", "horizontal"),
                weight=float(_get(t, "weight", 1.0)),
                priority=float(_get(t, "priority", 1.0)),
                max_runners=t.get("max_runners"),
                boot=Boot(
                    duration_ms=boot_raw["duration_ms"],
                    drain_grace_ms=_get(boot_raw, "drain_grace_ms", 20000),
                    timeout_ms=_get(boot_raw, "timeout_ms", 0),
                    gate_first=_get(boot_raw, "gate_first", True),
                    max_boots=_get(boot_raw, "max_boots", 500),
                    min_interval_ms=_get(boot_raw, "min_interval_ms", 0),
                    stat_interval_ms=_get(boot_raw, "stat_interval_ms", 5000),
                    stall_threshold_ms=_get(boot_raw, "stall_threshold_ms", 3000),
                    on_stall=_get(boot_raw, "on_stall", "report"),
                ),
                nightmare=Nightmare(
                    intensity=float(_get(nm_raw, "intensity", 0.5)),
                    seed_mode=_get(nm_raw, "seed_mode", "split"),
                    perturb=tuple(_get(nm_raw, "perturb", [])),
                    perturb_opts=dict(_get(nm_raw, "perturb_opts", {})),
                    opts=dict(_get(nm_raw, "opts", {})),
                ),
            )
        )

    if len(diags) != before:
        return None
    return Suite(meta, build, tuple(tasks))


try:  # pragma: no cover
    import jsonschema as _jsonschema

    SCHEMA_AVAILABLE = True
except ImportError:  # pragma: no cover
    _jsonschema = None
    SCHEMA_AVAILABLE = False


def _schema() -> dict[str, Any]:
    import json

    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def _pointer(err: Any) -> str:
    """A jsonschema error path as the TOML author wrote it."""
    out = ""
    for part in err.absolute_path:
        out += f"[{part}]" if isinstance(part, int) else (f".{part}" if out else part)
    return out or "<root>"


def schema_diagnostics(raw: dict) -> list[Diagnostic]:
    """Check against the published contract, when it can be checked here."""
    if not SCHEMA_AVAILABLE:
        return []
    validator = _jsonschema.Draft202012Validator(_schema())
    return [
        Diagnostic(_pointer(e), e.message)
        for e in sorted(validator.iter_errors(raw), key=lambda e: list(e.absolute_path))
    ]


def _enum(
    diags: list[Diagnostic], path: str, value: Any, allowed: Sequence[Any]
) -> None:
    if value not in allowed:
        diags.append(
            Diagnostic(path, f"{value!r} is not one of {', '.join(map(str, allowed))}")
        )


def coherence_diagnostics(s: Suite) -> list[Diagnostic]:
    d: list[Diagnostic] = []

    if not g.NAME_RE.match(s.meta.name):
        d.append(Diagnostic("suite.name", "must be lowercase [a-z][a-z0-9_]*"))
    if s.meta.runners < 1:
        d.append(Diagnostic("suite.runners", "must be at least 1"))
    if s.meta.budget_hours <= 0:
        d.append(Diagnostic("suite.budget_hours", "must be positive"))
    if not 0 <= s.meta.overlap_ratio < 1:
        d.append(Diagnostic("suite.overlap_ratio", "must be in [0, 1)"))

    _enum(d, "build.compiler", s.build.compiler, COMPILERS)
    _enum(d, "build.type", s.build.type, BUILD_TYPES)

    seen_defs: dict[str, str] = {}
    for i, definition in enumerate(s.build.cmake_definitions):
        if "=" not in definition:
            d.append(
                Diagnostic(
                    f"build.cmake_definitions[{i}]",
                    f"{definition!r} is not KEY=VALUE",
                )
            )
            continue
        key, value = definition.split("=", 1)
        if key in seen_defs:
            d.append(
                Diagnostic(
                    f"build.cmake_definitions[{i}]",
                    f"{key} is already defined as {seen_defs[key]!r}",
                )
            )
        seen_defs[key] = value

    asan = seen_defs.get("DEBUG_ASAN", "OFF").upper()
    if asan not in ("OFF", "0", "FALSE", "NO") and s.build.compiler != "clang":
        d.append(
            Diagnostic(
                "build.compiler",
                "DEBUG_ASAN needs clang",
            )
        )

    names: dict[str, int] = {}
    for i, t in enumerate(s.tasks):
        p = f"tasks[{i}]"
        if not g.NAME_RE.match(t.name):
            d.append(Diagnostic(f"{p}.name", "must be lowercase [a-z][a-z0-9_]*"))
        if t.name in names:
            d.append(
                Diagnostic(
                    f"{p}.name",
                    f"{t.name!r} already used by tasks[{names[t.name]}]",
                )
            )
        names[t.name] = i

        _enum(d, f"{p}.mode", t.mode, MODES)
        if t.weight <= 0:
            d.append(Diagnostic(f"{p}.weight", "must be positive"))
        if t.priority < 0:
            d.append(Diagnostic(f"{p}.priority", "must not be negative"))
        if t.max_runners is not None:
            if t.max_runners < 1:
                d.append(Diagnostic(f"{p}.max_runners", "must be at least 1"))
            elif t.max_runners > s.meta.runners:
                d.append(
                    Diagnostic(
                        f"{p}.max_runners",
                        f"{t.max_runners} exceeds suite.runners ({s.meta.runners})",
                    )
                )

        d += _boot_diagnostics(t.boot, s.meta, p)
        d += _nightmare_diagnostics(t.nightmare, f"{p}.nightmare")

    return d


def _boot_diagnostics(b: Boot, meta: SuiteMeta, p: str) -> list[Diagnostic]:
    """Validate boot budget and deadline constraints"""
    d: list[Diagnostic] = []
    _enum(d, f"{p}.boot.on_stall", b.on_stall, ON_STALL)

    if b.duration_ms <= 0:
        d.append(Diagnostic(f"{p}.boot.duration_ms", "must be positive"))
        return d
    if b.drain_grace_ms <= 0:
        d.append(Diagnostic(f"{p}.boot.drain_grace_ms", "must be positive"))
        return d
    if b.stat_interval_ms <= 0:
        d.append(Diagnostic(f"{p}.boot.stat_interval_ms", "must be positive"))
    if b.stall_threshold_ms < 100:
        d.append(Diagnostic(f"{p}.boot.stall_threshold_ms", "must be at least 100ms"))

    if b.timeout_ms and b.timeout_ms <= b.guest_hard_ms:
        d.append(
            Diagnostic(
                f"{p}.boot.timeout_ms",
                f"timeout_ms ({b.timeout_ms}ms) must exceed guest deadline ({b.guest_hard_ms}ms)",
            )
        )

    if b.stat_interval_ms >= b.duration_ms:
        d.append(
            Diagnostic(
                f"{p}.boot.stat_interval_ms",
                f"stat_interval_ms ({b.stat_interval_ms}ms) must be less than duration_ms ({b.duration_ms}ms)",
            )
        )

    if b.host_timeout_ms > meta.budget_ms:
        d.append(
            Diagnostic(
                f"{p}.boot.duration_ms",
                f"boot host timeout ({b.host_timeout_ms}ms) exceeds suite budget ({meta.budget_ms}ms)",
            )
        )

    if b.min_interval_ms >= b.host_timeout_ms:
        d.append(
            Diagnostic(
                f"{p}.boot.min_interval_ms",
                f"min_interval_ms ({b.min_interval_ms}ms) must be less than host_timeout_ms ({b.host_timeout_ms}ms)",
            )
        )

    return d


def _nightmare_diagnostics(n: Nightmare, p: str) -> list[Diagnostic]:
    d: list[Diagnostic] = []
    _enum(d, f"{p}.seed_mode", n.seed_mode, SEED_MODES)

    if not 0.0 <= n.intensity <= 1.0:
        d.append(Diagnostic(f"{p}.intensity", "must be in [0, 1]"))

    seen: set[str] = set()
    for i, svc in enumerate(n.perturb):
        if svc not in SERVICES:
            d.append(
                Diagnostic(
                    f"{p}.perturb[{i}]",
                    f"{svc!r} is not a known service ({', '.join(SERVICES)})",
                )
            )
        if svc in seen:
            d.append(Diagnostic(f"{p}.perturb[{i}]", f"{svc!r} is listed twice"))
        seen.add(svc)

    for svc, knobs in n.perturb_opts.items():
        if svc not in seen:
            d.append(
                Diagnostic(
                    f"{p}.perturb_opts.{svc}",
                    f"{svc!r} is configured in perturb_opts but not listed in perturb",
                )
            )
        if not isinstance(knobs, dict):
            d.append(Diagnostic(f"{p}.perturb_opts.{svc}", "expected a table"))
            continue
        for knob, value in knobs.items():
            if knob == "enabled":
                d.append(
                    Diagnostic(
                        f"{p}.perturb_opts.{svc}.enabled",
                        "'enabled' flag is invalid; list service in 'perturb' instead",
                    )
                )
                continue
            allowed = PERTURB_KNOBS.get(svc, frozenset())
            if knob not in allowed:
                d.append(
                    Diagnostic(
                        f"{p}.perturb_opts.{svc}.{knob}",
                        f"unknown knob; expected one of {', '.join(sorted(allowed))}",
                    )
                )
                continue
            d += _knob_diagnostics(value, f"{p}.perturb_opts.{svc}.{knob}", knob)

    for opt, value in n.opts.items():
        if not g.NAME_RE.match(opt):
            d.append(Diagnostic(f"{p}.opts.{opt}", "must be lowercase [a-z][a-z0-9_]*"))
        d += _knob_diagnostics(value, f"{p}.opts.{opt}", opt)

    return d


def _knob_diagnostics(value: Any, path: str, name: str) -> list[Diagnostic]:
    """Type a knob the way `render` will, so bad value fails at validation"""
    try:
        render_knob(value, name)
    except g.GrammarError as e:
        return [Diagnostic(path, str(e))]
    return []


def render_knob(value: Any, name: str) -> str:
    """Render one knob's value"""
    if isinstance(value, bool):
        return g.boolean(value)
    if isinstance(value, int):
        if name.endswith("_us"):
            return g.duration_us(value)
        if name.endswith("_ms"):
            return g.duration_ms(value)
        return g.uint(value)
    if isinstance(value, float):
        return g.fx(value)
    if isinstance(value, str):
        return value
    raise g.GrammarError(f"{type(value).__name__} is not a cmdline-renderable type")
