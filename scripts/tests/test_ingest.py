import gzip
import json
import tempfile
import unittest
from pathlib import Path

from charm import ingest as I
from charm import protocol as P


def ndjson_stream() -> str:
    def line(d, k, **f):
        return json.dumps({P.KEY_DOMAIN: d, P.KEY_KIND: k, **f})

    return (
        "\n".join(
            (
                line(P.DOMAIN_TEST, P.KIND_BEGIN, declared_total=1),
                line(P.DOMAIN_TEST, P.KIND_RESULT, name="a", status="pass"),
                line(
                    P.DOMAIN_TEST, P.KIND_TOTALS, total=1, passed=1, failed=0, skipped=0
                ),
                line(P.DOMAIN_NDJSON, P.KIND_BYE, code=0, reason="done"),
            )
        )
        + "\n"
    )


class ShardBuilder:
    def __init__(self, root: Path, name: str, stem: str = "run"):
        self.dir = root / name
        self.dir.mkdir(parents=True, exist_ok=True)
        self.stem = stem

    def write(self, *, env: dict, machine: str | None = None, gz: bool = False):
        (self.dir / f"{self.stem}.env").write_text(
            "".join(f"{k}={v}\n" for k, v in env.items())
        )
        console = self.dir / f"{self.stem}.log"
        console.write_text("console output\n")
        paths = [console]
        if machine is not None:
            m = self.dir / f"{self.stem}.nd.log"
            m.write_text(machine)
            paths.append(m)
        if gz:
            for p in paths:
                with p.open("rb") as src, gzip.open(f"{p}.gz", "wb") as dst:
                    dst.write(src.read())
                p.unlink()
        return self


class ParseEnvTests(unittest.TestCase):
    def test_shell_substitution_is_data_not_code(self) -> None:
        env = I.parse_env("SHARD=$(touch /tmp/pwned)\nSHA=`id`\n")

        self.assertEqual(env["SHARD"], "$(touch /tmp/pwned)")
        self.assertEqual(env["SHA"], "`id`")

    def test_unknown_keys_are_dropped(self) -> None:
        env = I.parse_env("SHARD=a\nLD_PRELOAD=/evil.so\n")

        self.assertEqual(env, {"SHARD": "a"})

    def test_values_may_be_empty(self) -> None:
        self.assertEqual(I.parse_env("SEED=\n"), {"SEED": ""})

    def test_a_quoted_value_loses_its_quotes(self) -> None:
        self.assertEqual(I.parse_env('SCENARIO="gate"\n')["SCENARIO"], "gate")


class IngestTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.artifacts = self.root / "artifacts"
        self.out = self.root / "ndjson"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_both_workflow_layouts_are_read_by_one_rule(self) -> None:
        # nightmare.yml uploads run.env and test.yml uploads iter-1.env
        ShardBuilder(self.artifacts, "nightmare-LOCKS", "run").write(
            env={"SHARD": "LOCKS", "EXIT_CODE": "0"}, machine=ndjson_stream()
        )
        ShardBuilder(self.artifacts, "results-gcc", "iter-1").write(
            env={"SHARD": "gcc-iter1", "EXIT_CODE": "0"}, machine=ndjson_stream()
        )

        written, problems = I.ingest(self.artifacts, self.out)

        self.assertEqual(len(written), 2)
        self.assertEqual([p for p in problems if p.level == "error"], [])

    def test_gzipped_logs_are_expanded(self) -> None:
        ShardBuilder(self.artifacts, "a").write(
            env={"SHARD": "a", "EXIT_CODE": "0"}, machine=ndjson_stream(), gz=True
        )

        written, problems = I.ingest(self.artifacts, self.out)

        self.assertEqual(len(written), 1)
        self.assertEqual([p for p in problems if p.level == "error"], [])

    def test_a_missing_machine_channel_is_an_error_but_keeps_the_shard(self) -> None:
        ShardBuilder(self.artifacts, "a").write(
            env={"SHARD": "a", "EXIT_CODE": "0"}, machine=None
        )

        written, problems = I.ingest(self.artifacts, self.out)

        self.assertEqual(len(written), 1)
        self.assertTrue(any("no NDJSON channel" in p.text for p in problems))

    def test_a_shard_name_cannot_escape_the_output_directory(self) -> None:
        ShardBuilder(self.artifacts, "a").write(
            env={"SHARD": "../../pwned", "EXIT_CODE": "0"}, machine=ndjson_stream()
        )

        written, problems = I.ingest(self.artifacts, self.out)

        self.assertEqual(written, [])
        self.assertFalse((self.root / "pwned.ndjson").exists())
        self.assertTrue(any("valid filename" in p.text for p in problems))

    def test_two_artifacts_claiming_one_shard_name_is_reported(self) -> None:
        ShardBuilder(self.artifacts, "a").write(
            env={"SHARD": "dup", "EXIT_CODE": "0"}, machine=ndjson_stream()
        )
        ShardBuilder(self.artifacts, "b").write(
            env={"SHARD": "dup", "EXIT_CODE": "0"}, machine=ndjson_stream()
        )

        written, problems = I.ingest(self.artifacts, self.out)

        self.assertEqual(len(written), 1)
        self.assertTrue(any("duplicate shard name" in p.text for p in problems))

    def test_an_empty_tree_is_an_error_not_a_clean_report(self) -> None:
        self.artifacts.mkdir(parents=True)

        written, problems = I.ingest(self.artifacts, self.out)

        self.assertEqual(written, [])
        self.assertTrue(any(p.level == "error" for p in problems))

    def test_an_unset_seed_is_absent_rather_than_empty(self) -> None:
        ShardBuilder(self.artifacts, "a").write(
            env={"SHARD": "a", "SEED": "", "EXIT_CODE": "0"}, machine=ndjson_stream()
        )

        written, _ = I.ingest(self.artifacts, self.out)
        run = json.loads(written[0].read_text().splitlines()[0])

        self.assertIsNone(run["seed"])

    def test_a_non_numeric_exit_code_does_not_abort_the_shard(self) -> None:
        ShardBuilder(self.artifacts, "a").write(
            env={"SHARD": "a", "EXIT_CODE": "n/a", "DURATION_S": "?"},
            machine=ndjson_stream(),
        )

        written, _ = I.ingest(self.artifacts, self.out)
        run = json.loads(written[0].read_text().splitlines()[0])

        self.assertIsNone(run["exit_code"])
        self.assertIsNone(run["duration_s"])


if __name__ == "__main__":
    unittest.main()
