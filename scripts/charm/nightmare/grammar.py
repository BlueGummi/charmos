"""values the kernel's cmdline parser accepts"""

import re
from collections.abc import Sequence
from typing import Final

MAX_VAR_LEN: Final = 128
MAX_VAL_LEN: Final = 256
MAX_CMDLINE_LEN: Final = 4096

DURATION_UNITS: Final = ("ns", "us", "ms", "s", "m", "h", "d")

BARE_VALUE_RE: Final = re.compile(r'^[^\s"\\]+$')
KEY_RE: Final = re.compile(r"^[A-Za-z_][A-Za-z0-9_.]*$")
NAME_RE: Final = re.compile(r"^[a-z][a-z0-9_]*$")
OPAQUE_RE: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]*$")


class GrammarError(ValueError):
    pass


def key(name: str) -> str:
    if not KEY_RE.match(name):
        raise GrammarError(f"{name!r} is not a valid cmdline key")
    if len(name) >= MAX_VAR_LEN:
        raise GrammarError(
            f"key {name!r} is {len(name)} bytes, but the kernel truncates at "
            f"{MAX_VAR_LEN - 1} (MAX_VAR_LEN)"
        )
    return name


def quote(value: str) -> str:
    """Render a value with escape chars"""
    if value == "":
        raise GrammarError("value cannot be empty")
    if BARE_VALUE_RE.match(value):
        rendered = value
    else:
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        rendered = f'"{escaped}"'
    if len(rendered) >= MAX_VAL_LEN:
        raise GrammarError(
            f"value is {len(rendered)} bytes, exceeds MAX_VAL_LEN ({MAX_VAL_LEN - 1})"
        )
    return rendered


def pair(name: str, value: str) -> str:
    return f"{key(name)}={quote(value)}"


def boolean(value: bool) -> str:
    return "true" if value else "false"


def uint(value: int) -> str:
    if value < 0:
        raise GrammarError(f"{value} is not unsigned")
    if value > 0xFFFFFFFFFFFFFFFF:
        raise GrammarError(f"{value} does not fit in u64")
    return str(value)


def parse_uint(text: str | int) -> int:
    try:
        val = int(text, 0) if isinstance(text, str) else int(text)
    except ValueError as e:
        raise GrammarError(f"{text!r} is not an integer") from e
    if not 0 <= val <= 0xFFFFFFFFFFFFFFFF:
        raise GrammarError(f"{val} does not fit in u64")
    return val


def hex_u64(value: int) -> str:
    if not 0 <= value <= 0xFFFFFFFFFFFFFFFF:
        raise GrammarError(f"{value} does not fit in u64")
    return f"0x{value:016x}"


def duration_ms(value: int) -> str:
    """must have an explicit unit"""
    if value < 0:
        raise GrammarError(f"{value}ms is negative")
    return f"{uint(value)}ms"


def duration_us(value: int) -> str:
    if value < 0:
        raise GrammarError(f"{value}us is negative")
    return f"{uint(value)}us"


def fx(value: float, *, places: int = 6) -> str:
    """Render a [0,1] fixed-point value"""
    if not 0.0 <= value <= 1.0:
        raise GrammarError(f"{value} is outside [0, 1]")
    text = f"{value:.{places}f}".rstrip("0")
    return f"{text}0" if text.endswith(".") else text


def name_list(values: Sequence[str]) -> str:
    if not values:
        raise GrammarError("list cannot be empty")
    for v in values:
        if not NAME_RE.match(v):
            raise GrammarError(f"{v!r} is not a valid service name")
    return ",".join(values)


def join(tokens: list[str]) -> str:
    line = " ".join(tokens)
    if len(line) > MAX_CMDLINE_LEN:
        raise GrammarError(
            f"command line length ({len(line)} bytes) exceeds limit of {MAX_CMDLINE_LEN} bytes"
        )
    return line
