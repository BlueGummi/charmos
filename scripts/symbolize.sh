#!/usr/bin/env bash

# Turn a charmOS panic backtrace into file:line

set -euo pipefail

SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="$SRC_ROOT/build/kernel/kernel"

usage() {
    cat >&2 <<EOF
usage: $0 [-k <kernel-elf>] [file]

  -k <elf>   kernel image to resolve against (default: $KERNEL)
  file       panic text; reads stdin when omitted
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -k|--kernel) [[ $# -ge 2 ]] || usage; KERNEL="$2"; shift 2 ;;
        -h|--help)   usage ;;
        --)          shift; break ;;
        -*)          usage ;;
        *)           break ;;
    esac
done
[[ $# -le 1 ]] || usage

if [[ $# -eq 1 && "$1" != "-" ]]; then
    [[ -r "$1" ]] || { echo "symbolize: cannot read $1" >&2; exit 1; }
    exec < "$1"
fi

[[ -f "$KERNEL" ]] || { echo "symbolize: no kernel image at $KERNEL" >&2; exit 1; }

SYMBOLIZER=""
for cand in llvm-symbolizer; do
    if command -v "$cand" >/dev/null 2>&1; then SYMBOLIZER="$cand"; break; fi
done
ADDR2LINE=""
if [[ -z "$SYMBOLIZER" ]]; then
    for cand in llvm-addr2line x86_64-elf-addr2line addr2line; do
        if command -v "$cand" >/dev/null 2>&1; then ADDR2LINE="$cand"; break; fi
    done
    [[ -n "$ADDR2LINE" ]] || {
        echo "symbolize: need llvm-symbolizer or addr2line on PATH" >&2
        exit 1
    }
fi

NM=""
for cand in llvm-nm x86_64-elf-nm nm; do
    if command -v "$cand" >/dev/null 2>&1; then NM="$cand"; break; fi
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# "<addr> <name>" for every text symbol, used to check the trace against the
# image it is being resolved against
if [[ -n "$NM" ]]; then
    LC_ALL=C "$NM" -n "$KERNEL" 2>/dev/null \
        | LC_ALL=C awk '/ [tT] / { print $3, $1 }' > "$TMP/syms" || : > "$TMP/syms"
else
    : > "$TMP/syms"
fi

# One address in, "func<TAB>file:line" per frame out, innermost inline first
resolve() {
    local addr="$1"
    if [[ -n "$SYMBOLIZER" ]]; then
        "$SYMBOLIZER" --obj="$KERNEL" --inlines --demangle \
                      --output-style=GNU "$addr" 2>/dev/null \
            | LC_ALL=C awk 'NF { if (n++ % 2 == 0) f = $0; else print f "\t" $0 }'
    else
        "$ADDR2LINE" -f -i -C -e "$KERNEL" "$addr" 2>/dev/null \
            | LC_ALL=C awk 'NF { if (n++ % 2 == 0) f = $0; else print f "\t" $0 }'
    fi
}

stale_warned=0

# A frame line looks like:  #5  [0xffffffff80026f5b] ext2_chmod+0x5b
frame_re='#([0-9]+)[[:space:]]+\[?(0x[0-9a-fA-F]+)\]?[[:space:]]+([A-Za-z_][A-Za-z0-9_.$]*)(\+0x([0-9a-fA-F]+))?'

while IFS= read -r line || [[ -n "$line" ]]; do
    clean="$(printf '%s' "$line" | LC_ALL=C sed $'s/\033\\[[0-9;]*[a-zA-Z]//g')"

    if [[ ! "$clean" =~ $frame_re ]]; then
        printf '%s\n' "$line"
        continue
    fi

    idx="${BASH_REMATCH[1]}"
    addr="${BASH_REMATCH[2]}"
    sym="${BASH_REMATCH[3]}"
    off="${BASH_REMATCH[5]:-0}"

    lookup="$addr"
    if [[ "$idx" != "0" ]]; then
        lookup="$(printf '0x%x' "$(( addr - 1 ))")"
    fi

    printf '%s\n' "$line"

    if [[ -s "$TMP/syms" && $stale_warned -eq 0 ]]; then
        base="$(LC_ALL=C awk -v s="$sym" '$1 == s { print $2; exit }' "$TMP/syms")"
        if [[ -n "$base" ]]; then
            expect=$(( 0x$base + 0x$off ))
            if [[ "$expect" -ne "$addr" ]]; then
                echo "        !! $sym+0x$off is 0x$(printf '%x' "$expect") in this image, trace says $addr" >&2
                echo "        !! stale build -- rebuild or point -k at the image that panicked" >&2
                stale_warned=1
            fi
        fi
    fi

    out="$(resolve "$lookup")"
    if [[ -z "$out" ]]; then
        printf '        (no debug info)\n'
        continue
    fi

    total="$(printf '%s\n' "$out" | LC_ALL=C grep -c . || true)"
    n=0
    while IFS=$'\t' read -r func loc; do
        [[ -n "$loc" ]] || continue
        n=$(( n + 1 ))
        loc="${loc#"$SRC_ROOT"/}"
        if [[ "$func" == "??" || "$loc" == "??"* ]]; then
            printf '        (no debug info)\n'
        elif [[ $n -lt $total ]]; then
            printf '        %s\t%s (inlined)\n' "$loc" "$func"
        else
            printf '        %s\t%s\n' "$loc" "$func"
        fi
    done <<< "$out"
done

exit 0
