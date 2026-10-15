"""Matrix rows for the orchestrator's build and runner jobs"""

from typing import Any


def build_matrix(plan_result: dict[str, Any], limit: int = 64) -> list[dict[str, str]]:
    if plan_result.get("kind") != "accepted":
        return []
    groups = plan_result["plan"]["buildGroups"]
    if len(groups) > limit:
        raise ValueError(f"build matrix has {len(groups)} rows; limit is {limit}")
    return [
        {
            "bundle_id": group["id"],
            "artifact": f"nightmare-build-{group['id']}",
        }
        for group in sorted(groups, key=lambda item: item["id"])
    ]


def runner_matrix(plan_bundle: dict[str, Any], limit: int = 64) -> list[dict[str, str]]:
    rows = plan_bundle["matrix"]
    if len(rows) > limit:
        raise ValueError(f"runner matrix has {len(rows)} rows; limit is {limit}")
    return [
        {
            "manifest_id": row["manifest_id"],
            "manifest_artifact": row["manifest_artifact"],
            "bundle_id": row["bundle_id"],
            "bundle_artifact": f"nightmare-build-{row['bundle_id']}",
        }
        for row in sorted(rows, key=lambda item: item["manifest_id"])
    ]
