#!/usr/bin/env bash

# Stamp the symbol table into an already linked kernel image

# Run from the kernel target's POST_BUILD, so the table is generated from, and
# written back into, the same file

set -euo pipefail

if [[ $# -ne 5 ]]; then
    echo "usage: $0 <nm> <objcopy> <syms.awk> <reserve-bytes> <kernel>" >&2
    exit 2
fi

NM="$1"
OBJCOPY="$2"
AWK_SCRIPT="$3"
RESERVE="$4"
KERNEL="$5"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

LC_ALL=C "$NM" -n "$KERNEL" \
    | LC_ALL=C awk -v RESERVE="$RESERVE" -f "$AWK_SCRIPT" > "$TMP/syms.bin"

actual=$(wc -c < "$TMP/syms.bin" | tr -d ' ')
if [[ "$actual" -ne "$RESERVE" ]]; then
    echo "stamp_syms: produced $actual bytes, .kernel_syms reserves $RESERVE" >&2
    exit 1
fi

"$OBJCOPY" --update-section ".kernel_syms=$TMP/syms.bin" "$KERNEL"
