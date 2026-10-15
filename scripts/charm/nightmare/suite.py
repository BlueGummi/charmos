"""Load, validate, and type check suite configs"""

import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, PrivateAttr, ValidationError

from ..machine import Smp
from . import grammar as g

PERTURB_KNOBS = {
    "migrator": frozenset({"interval_us"}),
    "waker": frozenset({"interval_us"}),
    "apc_spammer": frozenset({"interval_us"}),
    "stutter": frozenset({"period_ms", "gap_ms"}),
    "alloc_pressure": frozenset({"interval_us"}),
    "inject_armer": frozenset({"interval_us"}),
}
Service = Literal[
    "migrator", "waker", "apc_spammer", "stutter", "alloc_pressure", "inject_armer"
]

FLUSH_MARGIN_MS = 15000


@dataclass(frozen=True)
class Diagnostic:
    path: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}: {self.message}"


class SuiteError(ValueError):
    """A suite that cannot be loaded or does not cohere"""

    def __init__(self, source: str, diagnostics: list[Diagnostic]):
        self.source = source
        self.diagnostics = diagnostics
        body = "\n".join(f"  {d}" for d in diagnostics)
        super().__init__(f"{source}: {len(diagnostics)} problem(s)\n{body}")


class Model(BaseModel):
    model_config = ConfigDict(strict=True, frozen=True, extra="forbid")


class Build(Model):
    compiler: Literal["gcc", "clang"] = "gcc"
    type: Literal["Debug", "Release", "RelWithDebInfo", "MinSizeRel"] = "RelWithDebInfo"
    cmake_definitions: list[Annotated[str, Field(pattern=r"^[A-Za-z_]\w*=\S*$")]] = []
    smp: Smp = Smp()
    memory_mib: int = Field(2048, gt=0)


class Boot(Model):
    duration_ms: int = Field(gt=0)
    drain_grace_ms: int = Field(20000, gt=0)
    timeout_ms: int = Field(0, ge=0)  # 0 means "derive it"
    gate_first: bool = True
    max_boots: int = Field(500, ge=1)
    min_interval_ms: int = Field(0, ge=0)
    stat_interval_ms: int = Field(5000, gt=0)
    stall_threshold_ms: int = Field(3000, ge=100)
    on_stall: Literal["report", "crash"] = "report"

    @property
    def guest_hard_ms(self) -> int:
        return self.duration_ms + self.drain_grace_ms

    @property
    def host_timeout_ms(self) -> int:
        return self.timeout_ms or (self.guest_hard_ms + FLUSH_MARGIN_MS)


class Nightmare(Model):
    intensity: float = Field(0.5, ge=0, le=1)
    seed_mode: Literal["split", "seedful", "seedless"] = "split"
    perturb: list[Service] = []
    perturb_opts: dict[str, Any] = {}
    opts: dict[str, Any] = {}

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


class Task(Model):
    name: str = Field(pattern=g.NAME_RE.pattern)
    boot: Boot
    nightmare: Nightmare = Nightmare()
    mode: Literal["horizontal", "vertical"] = "horizontal"
    weight: float = Field(1.0, gt=0)
    priority: float = Field(1.0, ge=0)
    max_runners: int | None = Field(None, ge=1)


class SuiteMeta(Model):
    name: str = Field(pattern=g.NAME_RE.pattern)
    runners: int = Field(ge=1)
    budget_hours: float = Field(gt=0)

    @property
    def budget_ms(self) -> int:
        return int(self.budget_hours * 3600 * 1000)


class Suite(Model):
    meta: SuiteMeta = Field(alias="suite")
    build: Build = Build()
    tasks: list[Task] = Field(min_length=1)
    _source: str = PrivateAttr("<memory>")

    @property
    def source(self) -> str:
        return self._source

    def task(self, name: str) -> Task:
        for t in self.tasks:
            if t.name == name:
                return t
        known = ", ".join(t.name for t in self.tasks)
        raise KeyError(f"{self.source}: no task named {name!r} (have: {known})")

    def resolved(self) -> dict[str, Any]:
        """Defaults applied, so a manifest is independent of loader evolution"""
        return self.model_dump(mode="json", by_alias=True, exclude_none=True)


def load(path: Path) -> Suite:
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise SuiteError(
            str(path), [Diagnostic(path="<toml>", message=str(e))]
        ) from None
    except OSError as e:
        raise SuiteError(
            str(path), [Diagnostic(path="<file>", message=str(e))]
        ) from None
    return from_dict(raw, source=str(path))


def location(loc: tuple[int | str, ...]) -> str:
    """A pydantic error location as the TOML author wrote it."""
    out = ""
    for part in loc:
        out += f"[{part}]" if isinstance(part, int) else (f".{part}" if out else part)
    return out or "<root>"


def diagnostics_of(error: ValidationError, prefix: str = "") -> list[Diagnostic]:
    return [
        Diagnostic(
            path=location((prefix, *e["loc"]) if prefix else e["loc"]), message=e["msg"]
        )
        for e in error.errors()
    ]


def from_dict(raw: Any, source: str = "<memory>") -> Suite:
    try:
        suite = Suite.model_validate(raw)
    except ValidationError as error:
        raise SuiteError(source, diagnostics_of(error)) from None
    diags = coherence_diagnostics(suite)
    if diags:
        raise SuiteError(source, sorted(diags, key=lambda d: (d.path, d.message)))
    suite._source = source
    return suite


def coherence_diagnostics(s: Suite) -> list[Diagnostic]:
    """What a field's type cannot say on its own"""
    d: list[Diagnostic] = []

    def bad(path: str, message: str) -> None:
        d.append(Diagnostic(path=path, message=message))

    seen_defs: dict[str, str] = {}
    for i, definition in enumerate(s.build.cmake_definitions):
        key, value = definition.split("=", 1)
        if key in seen_defs:
            bad(
                f"build.cmake_definitions[{i}]",
                f"{key} is already defined as {seen_defs[key]!r}",
            )
        seen_defs[key] = value

    asan = seen_defs.get("DEBUG_ASAN", "OFF").upper()
    if asan not in ("OFF", "0", "FALSE", "NO") and s.build.compiler != "clang":
        bad("build.compiler", "DEBUG_ASAN needs clang")

    names: dict[str, int] = {}
    for i, t in enumerate(s.tasks):
        p = f"tasks[{i}]"
        if t.name in names:
            bad(f"{p}.name", f"{t.name!r} already used by tasks[{names[t.name]}]")
        names[t.name] = i
        if t.max_runners is not None and t.max_runners > s.meta.runners:
            bad(
                f"{p}.max_runners",
                f"{t.max_runners} exceeds suite.runners ({s.meta.runners})",
            )
        d += _boot_diagnostics(t.boot, s.meta, f"{p}.boot")
        d += _nightmare_diagnostics(t.nightmare, f"{p}.nightmare")

    return d


def _boot_diagnostics(b: Boot, meta: SuiteMeta, p: str) -> list[Diagnostic]:
    """Budget and deadline constraints"""
    checks = (
        (
            "timeout_ms",
            not b.timeout_ms or b.timeout_ms > b.guest_hard_ms,
            f"timeout_ms ({b.timeout_ms}ms) must exceed guest deadline ({b.guest_hard_ms}ms)",
        ),
        (
            "stat_interval_ms",
            b.stat_interval_ms < b.duration_ms,
            f"stat_interval_ms ({b.stat_interval_ms}ms) must be less than duration_ms ({b.duration_ms}ms)",
        ),
        (
            "duration_ms",
            b.host_timeout_ms <= meta.budget_ms,
            f"boot host timeout ({b.host_timeout_ms}ms) exceeds suite budget ({meta.budget_ms}ms)",
        ),
        (
            "min_interval_ms",
            b.min_interval_ms < b.host_timeout_ms,
            f"min_interval_ms ({b.min_interval_ms}ms) must be less than host_timeout_ms ({b.host_timeout_ms}ms)",
        ),
    )
    return [
        Diagnostic(path=f"{p}.{field}", message=message)
        for field, ok, message in checks
        if not ok
    ]


def _nightmare_diagnostics(n: Nightmare, p: str) -> list[Diagnostic]:
    d: list[Diagnostic] = []

    def bad(path: str, message: str) -> None:
        d.append(Diagnostic(path=f"{p}.{path}", message=message))

    seen: set[str] = set()
    for i, service in enumerate(n.perturb):
        if service in seen:
            bad(f"perturb[{i}]", f"{service!r} is listed twice")
        seen.add(service)

    for svc, knobs in n.perturb_opts.items():
        if svc not in seen:
            bad(
                f"perturb_opts.{svc}",
                f"{svc!r} is configured in perturb_opts but not listed in perturb",
            )
        if not isinstance(knobs, dict):
            bad(f"perturb_opts.{svc}", "expected a table")
            continue
        for knob, value in knobs.items():
            if knob == "enabled":
                bad(
                    f"perturb_opts.{svc}.enabled",
                    "'enabled' flag is invalid; list service in 'perturb' instead",
                )
                continue
            allowed = PERTURB_KNOBS.get(svc, frozenset())
            if knob not in allowed:
                bad(
                    f"perturb_opts.{svc}.{knob}",
                    f"unknown knob; expected one of {', '.join(sorted(allowed))}",
                )
                continue
            d += _knob_diagnostics(value, f"{p}.perturb_opts.{svc}.{knob}", knob)

    for opt, value in n.opts.items():
        if not g.NAME_RE.match(opt):
            bad(f"opts.{opt}", "must be lowercase [a-z][a-z0-9_]*")
        d += _knob_diagnostics(value, f"{p}.opts.{opt}", opt)

    return d


def _knob_diagnostics(value: Any, path: str, name: str) -> list[Diagnostic]:
    """Type a knob the way `render` will, so bad value fails at validation"""
    try:
        render_knob(value, name)
    except g.GrammarError as e:
        return [Diagnostic(path=path, message=str(e))]
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
