"""Convert boot-time schema dumps into JSON Schema."""

import json
import sys
from typing import Any

from . import protocol as P

SCHEMA_URL = "https://json-schema.org/draft/2020-12/schema"

TYPE_MAP = {
    P.TYPE_U64: {"type": "integer", "minimum": 0},
    P.TYPE_I64: {"type": "integer"},
    P.TYPE_BOOL: {"type": "boolean"},
    P.TYPE_STR: {"type": ["string", "null"]},
    P.TYPE_HEX: {"type": "string", "pattern": "^0x[0-9a-f]+$"},
}

ENVELOPE = {
    P.KEY_DOMAIN: {"type": "string"},
    P.KEY_KIND: {"type": "string"},
    P.KEY_VERSION: {"type": "integer", "minimum": 0},
    P.KEY_TIME: {"type": ["integer", "null"], "minimum": 0},
    P.KEY_CPU: {"type": ["integer", "null"], "minimum": 0},
    P.KEY_TRUNCATED: {"type": "boolean"},
}


SCHEMA_RECORD_FIELDS = frozenset(
    {"domain", "kind", "rec_version", "index", "field", "type"}
)


def collect(text: str) -> dict[tuple[str, str], dict[str, Any]]:
    """(domain, kind) -> {version, fields[]}, in declaration order"""
    records: dict[tuple[str, str], dict[str, Any]] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            rec.get(P.KEY_DOMAIN) != P.DOMAIN_NDJSON
            or rec.get(P.KEY_KIND) != P.KIND_SCHEMA
        ):
            continue
        if not SCHEMA_RECORD_FIELDS.issubset(rec):
            print("warning: ignoring malformed schema record", file=sys.stderr)
            continue

        entry = records.setdefault(
            (rec["domain"], rec["kind"]),
            {"version": rec["rec_version"], "fields": {}},
        )
        entry["fields"][rec["index"]] = (rec["field"], rec["type"])
    return records


def build(records: dict[tuple[str, str], dict[str, Any]]) -> dict[str, Any]:
    variants = []
    for (domain, kind), entry in sorted(records.items()):
        props = dict(ENVELOPE)
        props[P.KEY_DOMAIN] = {"const": domain}
        props[P.KEY_KIND] = {"const": kind}
        props[P.KEY_VERSION] = {"const": entry["version"]}

        required = [P.KEY_DOMAIN, P.KEY_KIND, P.KEY_VERSION, P.KEY_TIME, P.KEY_CPU]
        for index in sorted(entry["fields"]):
            name, type_name = entry["fields"][index]
            if type_name not in TYPE_MAP:
                print(
                    f"warning: {domain}/{kind} field {name} has unknown type "
                    f"{type_name!r}",
                    file=sys.stderr,
                )
                continue
            props[name] = TYPE_MAP[type_name]
            required.append(name)

        variants.append(
            {
                "title": f"{domain}/{kind}",
                "type": "object",
                "properties": props,
                "required": required,
                "additionalProperties": False,
            }
        )

    return {
        "$schema": SCHEMA_URL,
        "title": "CharmOS ndjson record",
        "description": "Generated from a boot with ndjson.schema=true. "
        "Do not edit by hand",
        "oneOf": variants,
    }
