"""Regression tests for report"""

import json
import unittest

from charm import report as RP
from charm import schema as S


class MarkdownEscapingTests(unittest.TestCase):
    def test_markdown_escapes_html_and_table_delimiters(self) -> None:
        runs = [{"_source": "run.ndjson", "outcome": "crash", "reached_summary": False}]
        crashes = [
            {
                "_source": "run.ndjson",
                "signature": "sig",
                "kind": "panic",
                "message": "</summary><script>alert(1)</script>",
                "frames": [],
            }
        ]
        tests = [{"name": "case|with`tick", "status": "fail"}]

        markdown = RP.render_markdown(RP.build_report(runs, tests, crashes, ""))

        self.assertIn("&lt;/summary&gt;&lt;script&gt;alert(1)&lt;/script&gt;", markdown)
        self.assertIn("case\\|with`tick", markdown)

    def test_a_newline_cannot_end_a_table_row(self) -> None:
        cell = RP.md_cell("first\nsecond")

        self.assertNotIn("\n", cell)

    def test_an_empty_cell_is_never_empty(self) -> None:
        self.assertEqual(RP.md_cell(None), "-")
        self.assertEqual(RP.md_cell(""), "-")


class VerdictTests(unittest.TestCase):
    def test_a_clean_run_is_ok(self) -> None:
        runs = [{"_source": "a.ndjson", "outcome": "pass", "reached_summary": True}]

        self.assertTrue(RP.build_report(runs, [], [], "")["ok"])

    def test_a_crash_with_no_failed_shard_still_fails_the_report(self) -> None:
        runs = [{"_source": "a.ndjson", "outcome": "pass", "reached_summary": True}]
        crashes = [{"_source": "a.ndjson", "signature": "sig", "kind": "panic"}]

        self.assertFalse(RP.build_report(runs, [], crashes, "")["ok"])

    def test_a_shard_count_mismatch_is_warned_about(self) -> None:
        runs = [
            {
                "_source": "a.ndjson",
                "outcome": "pass",
                "reached_summary": True,
                "tests_seen": 3,
                "declared_total": 5,
                "shard": "s",
            }
        ]

        warnings = RP.build_report(runs, [], [], "")["warnings"]

        self.assertTrue(any("declared 5" in w for w in warnings))

    def test_json_report_carries_every_unique_crash(self) -> None:
        runs = [{"_source": "a.ndjson", "outcome": "crash", "reached_summary": True}]
        crashes = [
            {"_source": "a.ndjson", "signature": "aa", "kind": "panic"},
            {"_source": "a.ndjson", "signature": "aa", "kind": "panic"},
            {"_source": "a.ndjson", "signature": "bb", "kind": "asan"},
        ]

        doc = json.loads(RP.render_json(RP.build_report(runs, [], crashes, "")))

        self.assertEqual(len(doc["unique_crashes"]), 2)
        self.assertEqual(doc["unique_crashes"][0]["count"], 2)


class SchemaTests(unittest.TestCase):
    def test_collect_ignores_malformed_schema_records(self) -> None:
        from charm import protocol as P

        text = json.dumps(
            {
                P.KEY_DOMAIN: P.DOMAIN_NDJSON,
                P.KEY_KIND: P.KIND_SCHEMA,
                "domain": "test",
            }
        )

        self.assertEqual(S.collect(text), {})


class ZeroIsNotAbsentTests(unittest.TestCase):
    def test_zero_renders_as_zero(self) -> None:
        self.assertEqual(RP.md_cell(0), "0")

    def test_none_still_renders_as_a_dash(self) -> None:
        self.assertEqual(RP.md_cell(None), "-")

    def test_a_clean_shard_shows_its_zero_counts(self) -> None:
        runs = [
            {
                "_source": "a.ndjson",
                "shard": "s",
                "outcome": "pass",
                "reached_summary": True,
                "tests_seen": 3,
                "totals": {"total": 3, "passed": 3, "failed": 0, "skipped": 0},
                "duration_s": 1.0,
            }
        ]

        markdown = RP.render_markdown(RP.build_report(runs, [], [], ""))
        row = next(ln for ln in markdown.splitlines() if "| s |" in ln)

        self.assertIn("| 3 | 0 | 0 |", row)


if __name__ == "__main__":
    unittest.main()
