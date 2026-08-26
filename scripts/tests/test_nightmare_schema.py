"""`nightmare/schemas/suite-v1.schema.json`

charmos-ci generates its Zod schema from that file

two failure modes:

  drift      -- the schema's vocabulary and the Python model's disagree
  vacuity    -- a schema that accepts everything passes every positive test

Skipped when `jsonschema` is not installed
"""

import json
import tomllib
import unittest

from charm.nightmare import suite as S
from charm.paths import nightmare_dir

SCHEMA = json.loads(S.SCHEMA_PATH.read_text())
FIXTURES = nightmare_dir() / "fixtures" / "suites"
SUITES = nightmare_dir() / "suites"


def base(**overrides) -> dict:
    doc = {
        "suite": {"name": "x", "runners": 1, "budget_hours": 1.0},
        "build": {},
        "tasks": [{"name": "t", "boot": {"duration_ms": 10000}}],
    }
    doc.update(overrides)
    return doc


class VocabularyTests(unittest.TestCase):
    def test_services_match(self) -> None:
        self.assertEqual(tuple(SCHEMA["$defs"]["service"]["enum"]), S.SERVICES)

    def test_compilers_match(self) -> None:
        enum = SCHEMA["$defs"]["build"]["properties"]["compiler"]["enum"]
        self.assertEqual(tuple(enum), S.COMPILERS)

    def test_build_types_match(self) -> None:
        enum = SCHEMA["$defs"]["build"]["properties"]["type"]["enum"]
        self.assertEqual(tuple(enum), S.BUILD_TYPES)

    def test_modes_match(self) -> None:
        enum = SCHEMA["$defs"]["task"]["properties"]["mode"]["enum"]
        self.assertEqual(tuple(enum), S.MODES)

    def test_seed_modes_match(self) -> None:
        enum = SCHEMA["$defs"]["nightmare"]["properties"]["seed_mode"]["enum"]
        self.assertEqual(tuple(enum), S.SEED_MODES)

    def test_on_stall_matches(self) -> None:
        enum = SCHEMA["$defs"]["boot"]["properties"]["on_stall"]["enum"]
        self.assertEqual(tuple(enum), S.ON_STALL)

    def test_schema_defaults_match_the_dataclass_defaults(self) -> None:
        boot = SCHEMA["$defs"]["boot"]["properties"]
        model = S.Boot(duration_ms=1)

        self.assertEqual(boot["drain_grace_ms"]["default"], model.drain_grace_ms)
        self.assertEqual(boot["max_boots"]["default"], model.max_boots)
        self.assertEqual(boot["min_interval_ms"]["default"], model.min_interval_ms)
        self.assertEqual(boot["stat_interval_ms"]["default"], model.stat_interval_ms)
        self.assertEqual(boot["gate_first"]["default"], model.gate_first)
        self.assertEqual(boot["on_stall"]["default"], model.on_stall)

    def test_nightmare_defaults_match(self) -> None:
        props = SCHEMA["$defs"]["nightmare"]["properties"]
        model = S.Nightmare()

        self.assertEqual(props["intensity"]["default"], model.intensity)
        self.assertEqual(props["seed_mode"]["default"], model.seed_mode)


@unittest.skipUnless(S.SCHEMA_AVAILABLE, "jsonschema is not installed")
class AcceptanceTests(unittest.TestCase):
    def check(self, doc: dict) -> list[S.Diagnostic]:
        return S.schema_diagnostics(doc)

    def test_every_valid_fixture_and_suite_validates(self) -> None:
        for path in sorted(
            list(FIXTURES.glob("valid/*.toml")) + list(SUITES.glob("*.toml"))
        ):
            with self.subTest(fixture=path.name):
                self.assertEqual(self.check(tomllib.loads(path.read_text())), [])

    def test_a_minimal_document_validates(self) -> None:
        self.assertEqual(self.check(base()), [])


@unittest.skipUnless(S.SCHEMA_AVAILABLE, "jsonschema is not installed")
class RejectionTests(unittest.TestCase):
    def rejects(self, doc: dict, expect_path: str) -> None:
        diags = S.schema_diagnostics(doc)
        self.assertTrue(diags, f"schema accepted {doc}")
        self.assertIn(expect_path, [d.path for d in diags])

    def test_an_unknown_top_level_key_is_rejected(self) -> None:
        self.rejects(base(surprise={}), "<root>")

    def test_an_unknown_suite_key_is_rejected(self) -> None:
        doc = base()
        doc["suite"]["budget_days"] = 3
        self.rejects(doc, "suite")

    def test_an_unknown_boot_key_is_rejected(self) -> None:
        doc = base()
        doc["tasks"][0]["boot"]["duration_sec"] = 3
        self.rejects(doc, "tasks[0].boot")

    def test_a_capitalised_name_is_rejected(self) -> None:
        doc = base()
        doc["suite"]["name"] = "Overnight_Locks"
        self.rejects(doc, "suite.name")

    def test_zero_runners_is_rejected(self) -> None:
        doc = base()
        doc["suite"]["runners"] = 0
        self.rejects(doc, "suite.runners")

    def test_an_intensity_above_one_is_rejected(self) -> None:
        doc = base()
        doc["tasks"][0]["nightmare"] = {"intensity": 1.5}
        self.rejects(doc, "tasks[0].nightmare.intensity")

    def test_an_unknown_service_is_rejected(self) -> None:
        doc = base()
        doc["tasks"][0]["nightmare"] = {"perturb": ["teleporter"]}
        self.rejects(doc, "tasks[0].nightmare.perturb[0]")

    def test_a_duplicated_service_is_rejected(self) -> None:
        doc = base()
        doc["tasks"][0]["nightmare"] = {"perturb": ["migrator", "migrator"]}
        self.rejects(doc, "tasks[0].nightmare.perturb")

    def test_an_empty_task_list_is_rejected(self) -> None:
        self.rejects(base(tasks=[]), "tasks")

    def test_a_cmake_definition_without_a_value_is_rejected(self) -> None:
        doc = base()
        doc["build"]["cmake_definitions"] = ["DEBUG_ASAN"]
        self.rejects(doc, "build.cmake_definitions[0]")

    def test_a_missing_required_field_is_rejected(self) -> None:
        doc = base()
        del doc["suite"]["runners"]
        self.rejects(doc, "suite")

    def test_a_string_where_a_number_belongs_is_rejected(self) -> None:
        doc = base()
        doc["tasks"][0]["boot"]["duration_ms"] = "10s"
        self.rejects(doc, "tasks[0].boot.duration_ms")


if __name__ == "__main__":
    unittest.main()
