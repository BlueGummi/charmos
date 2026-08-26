import re
import sys
from typing import Final

from .paths import repo_root

HEADER: Final = repo_root() / "include" / "ndjson.h"

DEFINE_RE = re.compile(
    r'^#define\s+(NDJSON_(?:KEY|DOMAIN|KIND|TYPE_NAME)_\w+)\s+"([^"]*)"\s*$',
    re.MULTILINE,
)


def _load() -> dict[str, str]:
    try:
        text = HEADER.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise SystemExit(f"charm.protocol: cannot read {HEADER}: {e}") from e

    found = dict(DEFINE_RE.findall(text))
    if not found:
        raise SystemExit(
            f"charm.protocol: {HEADER} defines no NDJSON_* wire names; "
            "has the header moved or the naming changed?"
        )
    return found


_CONSTS = _load()


def _c(name: str) -> str:
    try:
        return _CONSTS[name]
    except KeyError:
        raise SystemExit(f"charm.protocol: {name} is not defined in {HEADER}") from None


def _group(prefix: str) -> dict[str, str]:
    return {
        k[len(prefix) :].lower(): v for k, v in _CONSTS.items() if k.startswith(prefix)
    }


KEY_DOMAIN = _c("NDJSON_KEY_DOMAIN")
KEY_KIND = _c("NDJSON_KEY_KIND")
KEY_VERSION = _c("NDJSON_KEY_VERSION")
KEY_TIME = _c("NDJSON_KEY_TIME")
KEY_CPU = _c("NDJSON_KEY_CPU")
KEY_TRUNCATED = _c("NDJSON_KEY_TRUNCATED")

ENVELOPE_KEYS = frozenset(
    (KEY_DOMAIN, KEY_KIND, KEY_VERSION, KEY_TIME, KEY_CPU, KEY_TRUNCATED)
)

DOMAIN_NDJSON = _c("NDJSON_DOMAIN_NDJSON")
DOMAIN_TEST = _c("NDJSON_DOMAIN_TEST")
DOMAIN_PANIC = _c("NDJSON_DOMAIN_PANIC")
DOMAIN_ASAN = _c("NDJSON_DOMAIN_ASAN")

KIND_SCHEMA = _c("NDJSON_KIND_SCHEMA")
KIND_BYE = _c("NDJSON_KIND_BYE")
KIND_BEGIN = _c("NDJSON_KIND_BEGIN")
KIND_RESULT = _c("NDJSON_KIND_RESULT")
KIND_GROUP_START = _c("NDJSON_KIND_GROUP_START")
KIND_GROUP_END = _c("NDJSON_KIND_GROUP_END")
KIND_TOTALS = _c("NDJSON_KIND_TOTALS")
KIND_VERDICT = _c("NDJSON_KIND_VERDICT")
KIND_EXIT = _c("NDJSON_KIND_EXIT")
KIND_AT = _c("NDJSON_KIND_AT")
KIND_FAULT = _c("NDJSON_KIND_FAULT")
KIND_FRAME = _c("NDJSON_KIND_FRAME")
KIND_PEER = _c("NDJSON_KIND_PEER")
KIND_OWNER = _c("NDJSON_KIND_OWNER")

TYPE_U64 = _c("NDJSON_TYPE_NAME_U64")
TYPE_I64 = _c("NDJSON_TYPE_NAME_I64")
TYPE_BOOL = _c("NDJSON_TYPE_NAME_BOOL")
TYPE_STR = _c("NDJSON_TYPE_NAME_STR")
TYPE_HEX = _c("NDJSON_TYPE_NAME_HEX")

DOMAINS = _group("NDJSON_DOMAIN_")
KINDS = _group("NDJSON_KIND_")
TYPES = _group("NDJSON_TYPE_NAME_")


def dump(out=sys.stdout) -> None:
    """Print every wire name this build of the header declares."""
    print(f"header: {HEADER}", file=out)
    for title, group in (
        ("keys", _group("NDJSON_KEY_")),
        ("domains", DOMAINS),
        ("kinds", KINDS),
        ("types", TYPES),
    ):
        print(f"{title}:", file=out)
        for short, wire in sorted(group.items()):
            print(f"  {short:<14} {wire}", file=out)
