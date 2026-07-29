#!/usr/bin/env bash

# Usage: run_qemu.sh <logfile> <map-debug-exit:0|1> <qemu-binary> [qemu args...]

set -uo pipefail

LOGFILE="$1"; shift
MAP_DEBUG_EXIT="$1"; shift

"$@" 2>&1 | tee "$LOGFILE"
status="${PIPESTATUS[0]}"

if [[ "$MAP_DEBUG_EXIT" == "1" ]]; then
    case "$status" in
        1) exit 0 ;;   # TEST_EXIT_OK
        3) exit 1 ;;   # TEST_EXIT_FAIL
    esac
fi

exit "$status"
