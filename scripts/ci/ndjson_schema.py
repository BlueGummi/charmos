#!/usr/bin/env python3
"""

Parse the boot time schema dump

Usage:
    ndjson_schema.py ndjson.log --out record-v1.schema.json
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ndjson_protocol as P

SCHEMA_URL = "https://json-schema.org/draft/2020-12/schema"

TYPE_MAP = {
    P.TYPE_U64: {"type": "integer", "minimum": 0},
    P.TYPE_I64: {"type": "integer"},
    P.TYPE_BOOL: {"type": "boolean"},
    P.TYPE_STR: {"type": ["string", "null"]},
    P.TYPE_HEX: {"type": "string", "pattern": "^0x[0-9a-f]+$"},
}

# ``ndjson_emit()`` adds these to every record
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
    records = {}
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="an ndjson log from a run with ndjson.schema=true")
    ap.add_argument("--out", help="output path (default: stdout)")
    args = ap.parse_args()

    log_path = Path(args.log)
    records = collect(log_path.read_text(encoding="utf-8", errors="replace"))

    if not records:
        print(
            f"error: {log_path} carries no schema records; was it booted with "
            "ndjson.schema=true?",
            file=sys.stderr,
        )
        return 1

    schema = build(records)
    text = json.dumps(schema, indent=2, sort_keys=True) + "\n"

    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    print(
        f"described {len(records)} record types from {log_path}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
