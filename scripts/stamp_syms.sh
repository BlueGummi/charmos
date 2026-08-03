#!/usr/bin/env bash

# Stamp the symbol table into an already linked kernel image

# Run from the kernel target's POST_BUILD, so the table is generated from, and
# written back into, the same file

set -euo pipefail

if [[ $# -ne 5 ]]; then
    echo "usage: $0 <nm> <objcopy> <objdump> <reserve-bytes> <kernel>" >&2
    exit 2
fi

NM="$1"
OBJCOPY="$2"
OBJDUMP="$3"
RESERVE="$4"
KERNEL="$5"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

: > "$TMP/lines.bin"

if "$OBJDUMP" --dwarf=decodedline "$KERNEL" > "$TMP/decodedline" 2>/dev/null &&
   [[ -s "$TMP/decodedline" ]]; then
    if LC_ALL=C awk -v ROOT="$ROOT" -f "$HERE/decodedline.awk" \
           < "$TMP/decodedline" \
        | LC_ALL=C sort -k1,1 \
        | LC_ALL=C awk -f "$HERE/lines.awk" > "$TMP/lines.bin.tmp" 2>"$TMP/err"
    then
        mv "$TMP/lines.bin.tmp" "$TMP/lines.bin"
    else
        echo "stamp_syms: line table generation failed, panics will have no" \
             "file:line" >&2
        [[ -s "$TMP/err" ]] && sed 's/^/  /' "$TMP/err" >&2
        : > "$TMP/lines.bin"
    fi
else
    echo "stamp_syms: $OBJDUMP cannot dump DWARF line info, panics will have" \
         "no file:line" >&2
    echo "  (need GNU binutils objdump; llvm-objdump has no --dwarf)" >&2
fi

LINES_LEN=$(wc -c < "$TMP/lines.bin" | tr -d ' ')

LC_ALL=C "$NM" -n "$KERNEL" \
    | LC_ALL=C awk -v RESERVE="$RESERVE" -v LINES_LEN="$LINES_LEN" \
                   -f "$HERE/syms.awk" > "$TMP/syms.bin"

cat "$TMP/syms.bin" "$TMP/lines.bin" > "$TMP/blob.bin"

actual=$(wc -c < "$TMP/blob.bin" | tr -d ' ')
if [[ "$actual" -gt "$RESERVE" ]]; then
    echo "stamp_syms: produced $actual bytes, .kernel_syms reserves $RESERVE" >&2
    exit 1
fi

pad=$(( RESERVE - actual ))
if [[ "$pad" -gt 0 ]]; then
    dd if=/dev/zero bs="$pad" count=1 status=none >> "$TMP/blob.bin"
fi

"$OBJCOPY" --update-section ".kernel_syms=$TMP/blob.bin" "$KERNEL"

if [[ "$LINES_LEN" -gt 0 ]]; then
    printf 'stamp_syms: %s bytes of symbols + %s of line table, %s of %s used\n' \
           "$(wc -c < "$TMP/syms.bin" | tr -d ' ')" "$LINES_LEN" \
           "$actual" "$RESERVE"
fi
