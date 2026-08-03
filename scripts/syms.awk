function hexdigit(c) {
    return index("0123456789abcdef", tolower(c)) - 1
}

# 16 hex chars in, 8 bytes out, least significant first
function put_addr(h,    i) {
    while (length(h) < 16)
        h = "0" h

    for (i = 15; i >= 1; i -= 2)
        printf "%c", hexdigit(substr(h, i, 1)) * 16 + \
                     hexdigit(substr(h, i + 1, 1))
}

function put32(v,    i) {
    for (i = 0; i < 4; i++) {
        printf "%c", v % 256
        v = int(v / 256)
    }
}

BEGIN {
    if (RESERVE == "") {
        print "syms.awk: RESERVE not set" > "/dev/stderr"
        exit 1
    }

    HDR = 16
    ENT = 16
    n = 0
}

# nm -n gives "<addr> <type> <name>"; text symbols only
/ [tT] / {
    addr[n] = $1
    name[n] = $3
    n++
}

END {
    strtab_off = HDR + n * ENT

    # Offsets up front, so entries can point into a table not yet emitted
    off = 0
    for (i = 0; i < n; i++) {
        name_off[i] = off
        off += length(name[i]) + 1
    }

    total = strtab_off + off

    pad = (8 - total % 8) % 8
    lines_off = (LINES_LEN + 0) ? total + pad : 0

    if (total + pad + (LINES_LEN + 0) > RESERVE) {
        printf "syms.awk: table needs %d bytes, .kernel_syms reserves %d\n", \
               total + pad + (LINES_LEN + 0), RESERVE > "/dev/stderr"
        print "          raise KERNEL_SYMS_RESERVE in" \
              " include/linker/symbol_table.h" > "/dev/stderr"
        exit 1
    }

    # "SYMS"
    printf "%c%c%c%c", 83, 89, 77, 83
    put32(n)
    put32(strtab_off)
    put32(lines_off)

    for (i = 0; i < n; i++) {
        put_addr(addr[i])
        put32(name_off[i])
        put32(0)
    }

    for (i = 0; i < n; i++)
        printf "%s%c", name[i], 0

    for (i = 0; i < pad; i++)
        printf "%c", 0
}
