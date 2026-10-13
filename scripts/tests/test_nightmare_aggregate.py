import json
import tempfile
import unittest
from pathlib import Path
from typing import Any

from charm.nightmare import aggregate as A


def report(**discovery: int) -> A.AggregateReport:
    return A.AggregateReport(
        {
            "schema_version": 1,
            "plan_id": "plan_0",
            "batch_id": "batch_0",
            "ok": True,
            "partial": False,
            "expected_manifests": 1,
            "received_results": 1,
            "discovery": {
                "finding_count": 0,
                "unique_findings": 0,
                "crashed_boots": 0,
                "stalled_boots": 0,
                **discovery,
            },
            "infrastructure": {"issue_count": 0, "issues": []},
            "results": [],
            "findings": [],
        }
    )


class HeadlineTests(unittest.TestCase):
    def test_a_clean_batch_reads_clean(self) -> None:
        self.assertIn("## ✅", A.render_markdown(report()))

    def test_a_crashed_boot_is_not_a_clean_batch(self) -> None:
        markdown = A.render_markdown(report(crashed_boots=1))
        self.assertIn("## 🐛", markdown)
        self.assertIn("crashed boots: **1**", markdown)

    def test_a_stalled_boot_is_not_a_clean_batch(self) -> None:
        self.assertIn("## 🐛", A.render_markdown(report(stalled_boots=2)))

    def test_a_finding_is_not_a_clean_batch(self) -> None:
        self.assertIn("## 🐛", A.render_markdown(report(finding_count=1)))

    def test_a_broken_rig_outranks_discovery(self) -> None:
        broken = report(crashed_boots=1)
        broken.document["partial"] = True
        self.assertIn("## ⚠️", A.render_markdown(broken))


def runner_result(manifest_id: str, **summary: Any) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "result_id": "result_0",
        "manifest_id": manifest_id,
        "manifest_sha256": "0" * 64,
        "lifecycle": "completed",
        "discovery": {"kind": "crash", "finding_count": 0},
        "execution": {"health": "healthy", "code": "completed", "message": ""},
        "started_at": "2026-09-22T00:00:00Z",
        "ended_at": "2026-09-22T00:01:00Z",
        "campaign": {"summary": summary, "findings": [], "trace": []},
        "artifacts": {},
        "replay": {"argv": []},
    }


class DiscoveryRollupTests(unittest.TestCase):
    def aggregate(self, results: list[dict[str, Any]]) -> A.AggregateReport:
        plan = {
            "kind": "accepted",
            "plan": {
                "id": "plan_0",
                "batchId": "batch_0",
                "manifests": [
                    {"manifestId": r["manifest_id"], "buildGroupId": "build_0"}
                    for r in results
                ],
                "buildGroups": [{"id": "build_0"}],
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for index, result in enumerate(results):
                leg = root / f"leg-{index}"
                leg.mkdir()
                (leg / "runner_result.json").write_text(
                    json.dumps(result), encoding="utf-8"
                )
            return A.aggregate(plan, results_dir=root)

    def test_crashed_boots_reach_the_batch_report(self) -> None:
        document = self.aggregate(
            [
                runner_result("manifest_a", crashed_boots=1, stalled_boots=0),
                runner_result("manifest_b", crashed_boots=2, stalled_boots=3),
            ]
        ).document

        self.assertEqual(document["discovery"]["crashed_boots"], 3)
        self.assertEqual(document["discovery"]["stalled_boots"], 3)

    def test_each_leg_carries_its_own_crash_count(self) -> None:
        document = self.aggregate(
            [runner_result("manifest_a", crashed_boots=1)]
        ).document

        self.assertEqual(document["results"][0]["crashed_boots"], 1)

    def test_a_runner_from_before_the_counts_existed_is_read_as_zero(self) -> None:
        document = self.aggregate([runner_result("manifest_a")]).document

        self.assertEqual(document["discovery"]["crashed_boots"], 0)


if __name__ == "__main__":
    unittest.main()
