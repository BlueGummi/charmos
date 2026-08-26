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
        1)  exit 0 ;;  # 0: nightmare/test OK
        3)  exit 1 ;;  # 1: gate test failure
        5)  exit 3 ;;  # 2: panic
        7)  exit 3 ;;  # 3: nightmare finding
        9)  exit 4 ;;  # 4: nightmare harness/subject failure
        11) exit 5 ;;  # 5: nightmare stall
        13) exit 6 ;;  # 6: nightmare refusal/skip
    esac
fi

exit "$status"
