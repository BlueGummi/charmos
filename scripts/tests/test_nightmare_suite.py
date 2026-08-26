"""Suite tests: defaults, and the incoherent configs that fail"""

import unittest
from pathlib import Path

from charm.nightmare import suite as S
from charm.paths import nightmare_dir

FIXTURES = nightmare_dir() / "fixtures" / "suites"
SUITES = nightmare_dir() / "suites"


def expectation(path: Path) -> tuple[str, str]:
    lines = path.read_text().splitlines()
    expect = lines[0].removeprefix("#! expect:").strip()
    because = lines[1].removeprefix("#! because:").strip()
    return expect, because


class FixtureTests(unittest.TestCase):
    def test_there_are_fixtures_to_run(self) -> None:
        self.assertTrue(list(FIXTURES.glob("valid/*.toml")))
        self.assertTrue(list(FIXTURES.glob("invalid/*.toml")))

    def test_every_valid_fixture_loads(self) -> None:
        for path in sorted(FIXTURES.glob("valid/*.toml")):
            with self.subTest(fixture=path.name):
                S.load(path)

    def test_every_committed_suite_loads(self) -> None:
        for path in sorted(SUITES.glob("*.toml")):
            with self.subTest(suite=path.name):
                S.load(path)

    def test_every_invalid_fixture_fails_at_the_field_it_names(self) -> None:
        for path in sorted(FIXTURES.glob("invalid/*.toml")):
            with self.subTest(fixture=path.name):
                expect, because = expectation(path)
                with self.assertRaises(S.SuiteError) as cm:
                    S.load(path)
                paths = [d.path for d in cm.exception.diagnostics]
                self.assertIn(
                    expect,
                    paths,
                    f"{path.name} should fail at {expect} ({because}); got {paths}",
                )


class DefaultTests(unittest.TestCase):
    def suite(self) -> S.Suite:
        return S.load(FIXTURES / "valid" / "minimal.toml")

    def test_a_minimal_suite_gets_every_default(self) -> None:
        t = self.suite().tasks[0]

        self.assertEqual(t.mode, "horizontal")
        self.assertEqual(t.nightmare.seed_mode, "split")
        self.assertEqual(t.boot.drain_grace_ms, 20000)
        self.assertEqual(t.boot.max_boots, 500)
        self.assertTrue(t.boot.gate_first)

    def test_the_host_timeout_is_derived_outside_the_guest_hard_deadline(self) -> None:
        t = self.suite().tasks[0]

        self.assertEqual(t.boot.guest_hard_ms, 10000 + 20000)
        self.assertGreater(t.boot.host_timeout_ms, t.boot.guest_hard_ms)
        self.assertEqual(
            t.boot.host_timeout_ms, t.boot.guest_hard_ms + S.FLUSH_MARGIN_MS
        )

    def test_an_explicit_timeout_wins_over_the_derived_one(self) -> None:
        s = S.from_dict(
            {
                "suite": {"name": "x", "runners": 1, "budget_hours": 1.0},
                "build": {},
                "tasks": [
                    {"name": "t", "boot": {"duration_ms": 10000, "timeout_ms": 90000}}
                ],
            }
        )

        self.assertEqual(s.tasks[0].boot.host_timeout_ms, 90000)

    def test_smp_topology_renders_the_way_cmake_wants_it(self) -> None:
        s = S.load(SUITES / "overnight_locks.toml")

        self.assertEqual(s.build.smp.topo(), "sockets=2,cores=2,threads=2")
        self.assertEqual(s.build.smp.total, 8)


class SeedModeTests(unittest.TestCase):
    def mode(self, name: str) -> S.Nightmare:
        return S.Nightmare(seed_mode=name)

    def test_split_seeds_the_pressure_but_not_the_subject(self) -> None:
        n = self.mode("split")

        self.assertTrue(n.seeds_perturbers)
        self.assertFalse(n.seeds_subject)
        self.assertTrue(n.wants_seed)

    def test_seedful_seeds_both(self) -> None:
        n = self.mode("seedful")

        self.assertTrue(n.seeds_perturbers)
        self.assertTrue(n.seeds_subject)

    def test_seedless_wants_no_seed_at_all(self) -> None:
        n = self.mode("seedless")

        self.assertFalse(n.seeds_perturbers)
        self.assertFalse(n.wants_seed)


class DiagnosticTests(unittest.TestCase):
    def bad(self, doc: dict) -> list[S.Diagnostic]:
        with self.assertRaises(S.SuiteError) as cm:
            S.from_dict(doc)
        return cm.exception.diagnostics

    def test_all_problems_are_reported_together(self) -> None:
        diags = self.bad(
            {
                "suite": {"name": "Bad Name", "runners": 0, "budget_hours": -1},
                "build": {"compiler": "tcc"},
                "tasks": [{"name": "t", "boot": {"duration_ms": 10000}}],
            }
        )
        paths = {d.path for d in diags}

        self.assertIn("suite.name", paths)
        self.assertIn("suite.runners", paths)
        self.assertIn("suite.budget_hours", paths)
        self.assertIn("build.compiler", paths)

    def test_a_missing_required_field_names_itself(self) -> None:
        diags = self.bad({"suite": {"name": "x"}, "build": {}, "tasks": []})

        self.assertTrue(any("suite.runners" in d.path for d in diags))

    def test_a_task_without_a_duration_is_refused(self) -> None:
        diags = self.bad(
            {
                "suite": {"name": "x", "runners": 1, "budget_hours": 1.0},
                "build": {},
                "tasks": [{"name": "t", "boot": {}}],
            }
        )

        self.assertIn("tasks[0].boot.duration_ms", [d.path for d in diags])

    def test_a_knob_with_an_unrenderable_type_fails_at_validate_time(self) -> None:
        diags = self.bad(
            {
                "suite": {"name": "x", "runners": 1, "budget_hours": 1.0},
                "build": {},
                "tasks": [
                    {
                        "name": "t",
                        "boot": {"duration_ms": 10000},
                        "nightmare": {"opts": {"weird": [1, 2, 3]}},
                    }
                ],
            }
        )

        self.assertIn("tasks[0].nightmare.opts.weird", [d.path for d in diags])


class KnobTypingTests(unittest.TestCase):
    def test_a_us_suffix_becomes_a_microsecond_duration(self) -> None:
        self.assertEqual(S.render_knob(500, "interval_us"), "500us")

    def test_a_ms_suffix_becomes_a_millisecond_duration(self) -> None:
        self.assertEqual(S.render_knob(3000, "period_ms"), "3000ms")

    def test_a_plain_integer_stays_an_integer(self) -> None:
        self.assertEqual(S.render_knob(7, "burst"), "7")

    def test_a_float_becomes_fixed_point(self) -> None:
        self.assertEqual(S.render_knob(0.25, "aggression"), "0.25")

    def test_a_bool_is_rendered_the_way_the_kernel_reads_it(self) -> None:
        self.assertEqual(S.render_knob(True, "flag"), "true")
        self.assertEqual(S.render_knob(False, "flag"), "false")


if __name__ == "__main__":
    unittest.main()
