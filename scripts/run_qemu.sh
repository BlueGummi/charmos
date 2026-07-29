#!/usr/bin/env bash


set -uo pipefail

LOGFILE="$1"; shift
MAP_DEBUG_EXIT="$1"; shift

# QEMU's diagnostics end up in stderr
ERRLOG="${LOGFILE%.*}.stderr.log"

# Still shown live on the terminal, 
# host and guest are never confused for one another
"$@" 2> >(tee "$ERRLOG" | sed 's/^/[qemu] /' >&2) | tee "$LOGFILE"
status="${PIPESTATUS[0]}"

if [[ "$MAP_DEBUG_EXIT" == "1" ]]; then
    case "$status" in
        1) exit 0 ;;   # TEST_EXIT_OK
        3) exit 1 ;;   # TEST_EXIT_FAIL
    esac
fi

exit "$status"
