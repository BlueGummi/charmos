from charm.nightmare.aggregate import (
    check_execution_parity,
    normalize_signature,
    verify_finding_parity,
    verify_trace_monotonicity,
)


def test_normalize_signature() -> None:
    assert normalize_signature("LOCK_INVARIANT") == "lock_invariant"
    assert normalize_signature({"lane": "locks", "check": "storm"}) == "locks:storm"


def test_verify_trace_monotonicity() -> None:
    valid_trace = [
        {"at_ms": 0, "cumulative_progress": 0},
        {"at_ms": 1000, "cumulative_progress": 10},
        {"at_ms": 2000, "cumulative_progress": 25},
    ]
    assert verify_trace_monotonicity(valid_trace) is True

    regressed_trace = [
        {"at_ms": 0, "cumulative_progress": 10},
        {"at_ms": 1000, "cumulative_progress": 5},
    ]
    assert verify_trace_monotonicity(regressed_trace) is False

    time_travel_trace = [
        {"at_ms": 1000, "cumulative_progress": 10},
        {"at_ms": 500, "cumulative_progress": 20},
    ]
    assert verify_trace_monotonicity(time_travel_trace) is False


def test_verify_finding_parity() -> None:
    static_findings = [
        {"signature": "lock_invariant"},
        {"signature": "kasan_use_after_free"},
    ]
    orch_findings = [
        {"signature": "LOCK_INVARIANT"},
        {"signature": "kasan_use_after_free"},
    ]
    ok, diffs = verify_finding_parity(static_findings, orch_findings)
    assert ok is True
    assert diffs == []

    mismatched_orch = [{"signature": "lock_invariant"}]
    ok, diffs = verify_finding_parity(static_findings, mismatched_orch)
    assert ok is False
    assert len(diffs) == 1


def test_check_execution_parity() -> None:
    static_report = {
        "ok": True,
        "findings": [{"signature": "lock_invariant"}],
    }
    orch_report = {
        "ok": True,
        "partial": False,
        "findings": [{"signature": "lock_invariant"}],
        "results": [
            {
                "manifest_id": "manifest_1",
                "trace": [
                    {"at_ms": 0, "cumulative_progress": 0},
                    {"at_ms": 100, "cumulative_progress": 5},
                ],
            }
        ],
    }
    verdict = check_execution_parity(static_report, orch_report)
    assert verdict.matches is True
    assert verdict.findings_count == 1
    assert verdict.traces_consistent is True
    assert verdict.infrastructure_ok is True
