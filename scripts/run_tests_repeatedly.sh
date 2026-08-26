#!/usr/bin/env bash

set -u -o pipefail

runs="${1:-1}"
timeout_seconds="${2:-60}"

if ! [[ "$runs" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-run-count] [positive-timeout-seconds]" >&2
    exit 2
fi

if ! [[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-run-count] [positive-timeout-seconds]" >&2
    exit 2
fi

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$repo_dir/build"
batch="$(date +%Y%m%d-%H%M%S)"
results_dir="$build_dir/repeated-test-runs/$batch"
mkdir -p "$results_dir"

for ((run = 1; run <= runs; run++)); do
    runner_log="$results_dir/run-${run}.runner.log"
    guest_log="$results_dir/run-${run}.guest.log"
    qemu_log="$results_dir/run-${run}.qemu.log"
    machine_log="$results_dir/run-${run}.nd.log"
    results="$results_dir/run-${run}.results.ndjson"

    echo "[$run/$runs] ninja tests"
    (
        cd "$build_dir" || exit 2
        timeout --foreground --kill-after=5s "${timeout_seconds}s" \
            ninja tests >"$runner_log" 2>&1
    )
    status=$?

    if [[ -f "$build_dir/output.log" ]]; then
        cp "$build_dir/output.log" "$guest_log"
    fi
    if [[ -f "$build_dir/output.stderr.log" ]]; then
        cp "$build_dir/output.stderr.log" "$qemu_log"
    fi
    if [[ -f "$build_dir/ndjson.log" ]]; then
        cp "$build_dir/ndjson.log" "$machine_log"
    fi

    if [[ $status -ne 0 ]]; then
        echo "[$run/$runs] FAIL (status $status)" >&2
        if [[ -f "$machine_log" ]]; then
            python3 "$repo_dir/scripts/ci/parse_log.py" "$guest_log" \
                --ndjson "$machine_log" \
                --shard "run-$run" \
                --exit-code "$status" \
                --out "$results" >&2
            python3 "$repo_dir/scripts/ci/aggregate.py" "$results" >&2
        else
            echo "no machine channel at $machine_log, nothing to report" >&2
        fi
        exit "$status"
    fi

    echo "[$run/$runs] PASS"
done

echo "$runs/$runs consecutive runs passed"
