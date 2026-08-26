"""Regression tests for the NDJSON reader"""

import json
import unittest
from io import StringIO

from charm import protocol as P
from charm import record as R


def line(wire_domain: str, wire_kind: str, **fields: object) -> str:
    return json.dumps({P.KEY_DOMAIN: wire_domain, P.KEY_KIND: wire_kind, **fields})


class ParseNdjsonTests(unittest.TestCase):
    def test_malformed_schema_record_is_counted_not_raised(self) -> None:
        result = R.parse_ndjson(
            StringIO(line(P.DOMAIN_NDJSON, P.KIND_SCHEMA, domain="test"))
        )

        self.assertEqual(result.records, [])
        self.assertEqual(result.stats.malformed_lines, 1)
        self.assertFalse(result.stats.schema_seen)

    def test_crash_frames_attach_to_the_preceding_crash(self) -> None:
        stream = "\n".join(
            (
                line(
                    P.DOMAIN_PANIC,
                    P.KIND_AT,
                    file="foo.c",
                    line=12,
                    func="boom",
                    msg="bad",
                ),
                line(
                    P.DOMAIN_PANIC, P.KIND_FRAME, idx=0, addr="0x1", sym="boom", off=0
                ),
            )
        )

        result = R.parse_ndjson(StringIO(stream))

        self.assertEqual(result.stats.crashes, 1)
        self.assertEqual(
            result.records[0]["frames"],
            [{"index": 0, "address": "0x1", "symbol": "boom", "offset": 0}],
        )
        self.assertIn("signature", result.records[0])

    def test_a_truncated_line_costs_one_record_not_the_run(self) -> None:
        stream = "\n".join(
            (
                line(P.DOMAIN_TEST, P.KIND_RESULT, name="a", status="pass"),
                '{"d":"test","k":"resu',
                line(P.DOMAIN_TEST, P.KIND_RESULT, name="b", status="pass"),
            )
        )

        result = R.parse_ndjson(StringIO(stream))

        self.assertEqual(result.stats.tests_seen, 2)
        self.assertEqual(result.stats.malformed_lines, 1)

    def test_signature_ignores_addresses_and_counters(self) -> None:
        a = R.crash_signature("panic", "f.c:1", "bad ptr 0xdeadbeef after 12 ops", [])
        b = R.crash_signature("panic", "f.c:1", "bad ptr 0xcafe after 900 ops", [])

        self.assertEqual(a, b)


class ClassifyTests(unittest.TestCase):
    def clean(self, **kw) -> R.ParseStats:
        base = {
            "ended": True,
            "reached_summary": True,
            "totals": {"total": 1, "passed": 1, "failed": 0, "skipped": 0},
        }
        return R.ParseStats(**{**base, **kw})

    def test_clean_run_passes(self) -> None:
        self.assertEqual(R.classify(self.clean(), 0), "pass")

    def test_a_crash_outranks_a_nonzero_exit(self) -> None:
        self.assertEqual(R.classify(self.clean(crashes=1), 1), "crash")

    def test_panic_exit_is_a_crash_even_with_no_crash_record(self) -> None:
        self.assertEqual(R.classify(self.clean(), R.EXIT_PANIC), "crash")

    def test_a_guest_that_never_said_goodbye_is_incomplete_not_a_pass(self) -> None:
        self.assertEqual(R.classify(self.clean(ended=False), 0), "incomplete")

    def test_reaching_no_summary_is_incomplete_not_a_pass(self) -> None:
        self.assertEqual(R.classify(self.clean(reached_summary=False), 0), "incomplete")

    def test_failed_tests_fail(self) -> None:
        stats = self.clean(totals={"total": 2, "passed": 1, "failed": 1, "skipped": 0})
        self.assertEqual(R.classify(stats, 0), "fail")


class RunRecordTests(unittest.TestCase):
    def test_host_identity_reaches_the_record(self) -> None:
        result = R.parse_ndjson(StringIO(""))
        meta = R.RunMeta(shard="s1", seed="0xdead", compiler="clang", exit_code=0)

        run = R.build_run_record(result, meta, log_bytes=42)

        self.assertEqual(run["shard"], "s1")
        self.assertEqual(run["seed"], "0xdead")
        self.assertEqual(run["log_bytes"], 42)

    def test_an_unended_run_records_where_it_stopped(self) -> None:
        stream = line(P.DOMAIN_TEST, P.KIND_RESULT, name="a", status="pass")
        result = R.parse_ndjson(StringIO(stream))

        run = R.build_run_record(result, R.RunMeta(), 0)

        self.assertFalse(run["guest_ended"])
        self.assertEqual(run["last_record"], f"{P.DOMAIN_TEST}/{P.KIND_RESULT}")
        self.assertNotIn("guest_exit_code", run)

    def test_render_stamps_the_format_version(self) -> None:
        text = R.render([{"type": "run"}])

        self.assertEqual(json.loads(text)["v"], R.RESULT_FORMAT_VERSION)


class SchemaDriftTests(unittest.TestCase):
    def test_a_field_the_kernel_stopped_declaring_is_reported(self) -> None:
        schema = {
            (P.DOMAIN_TEST, P.KIND_TOTALS): {
                "version": 1,
                "fields": [{"name": "total"}, {"name": "passed"}],
            }
        }

        errors = R.ndjson_schema_check(schema)

        self.assertEqual(len(errors), 1)
        self.assertIn("failed", errors[0])

    def test_a_version_bump_is_reported_before_fields(self) -> None:
        schema = {
            (P.DOMAIN_TEST, P.KIND_TOTALS): {"version": 2, "fields": []},
        }

        errors = R.ndjson_schema_check(schema)

        self.assertEqual(len(errors), 1)
        self.assertIn("v1", errors[0])
        self.assertIn("v2", errors[0])


if __name__ == "__main__":
    unittest.main()
