# Encode an address -> file:line table for .kernel_syms
#
# Input is decodedline.awk output, sorted by address:
#
#   ffffffff80001000<TAB>93<TAB>kernel/acpi/dmar.c
#
# no lookup index: the reader is a panic path where a walk
# of a couple hundred KB is free, and one less thing to get wrong

function hexdigit(c) {
    return index("0123456789abcdef", tolower(c)) - 1
}

function hexnum(h,    i, v) {
    v = 0
    for (i = 1; i <= length(h); i++)
        v = v * 16 + hexdigit(substr(h, i, 1))
    return v
}

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

function put_uleb(v,    b) {
    while (1) {
        b = v % 128
        v = int(v / 128)
        if (v) {
            printf "%c", b + 128
        } else {
            printf "%c", b
            break
        }
    }
}

function uleb_len(v,    n) {
    n = 1
    while (v >= 128) {
        v = int(v / 128)
        n++
    }
    return n
}

function zig(v) {
    return (v < 0) ? (-v) * 2 - 1 : v * 2
}

BEGIN {
    FS = "\t"
    HDR = 32
    n = 0
    nfiles = 0
}

{
    hi = substr($1, 1, 8)
    lo = substr($1, 9, 8)

    if (n == 0) {
        base_hex = $1
        base_hi = hi
        base_lo = hexnum(lo)
    } else if (hi != base_hi) {
        print "lines.awk: address " $1 " leaves the " base_hi \
              " region; the delta encoding assumes one high half" \
              > "/dev/stderr"
        exit 1
    }

    a = hexnum(lo)

    if (n > 0 && a == addr[n - 1])
        next

    f = $3
    if (!(f in file_idx)) {
        file_idx[f] = nfiles
        file_name[nfiles] = f
        nfiles++
    }

    fi = file_idx[f]
    ln = $2 + 0

    if (n > 0 && fi == prev_fi && ln == prev_ln)
        next

    addr[n] = a
    fidx[n] = fi
    line[n] = ln
    n++

    prev_fi = fi
    prev_ln = ln
}

END {
    if (n == 0) {
        print "lines.awk: no line rows on stdin" > "/dev/stderr"
        exit 1
    }

    stream_len = 0
    pa = base_lo; pf = 0; pl = 0
    for (i = 0; i < n; i++) {
        stream_len += uleb_len(addr[i] - pa)
        stream_len += uleb_len(zig(fidx[i] - pf))
        stream_len += uleb_len(zig(line[i] - pl))
        pa = addr[i]; pf = fidx[i]; pl = line[i]
    }

    files_len = 0
    for (i = 0; i < nfiles; i++)
        files_len += length(file_name[i]) + 1

    # "LINE"
    printf "%c%c%c%c", 76, 73, 78, 69
    put32(n)
    put_addr(base_hex)
    put32(HDR)
    put32(stream_len)
    put32(HDR + stream_len)
    put32(files_len)

    pa = base_lo; pf = 0; pl = 0
    for (i = 0; i < n; i++) {
        put_uleb(addr[i] - pa)
        put_uleb(zig(fidx[i] - pf))
        put_uleb(zig(line[i] - pl))
        pa = addr[i]; pf = fidx[i]; pl = line[i]
    }

    for (i = 0; i < nfiles; i++)
        printf "%s%c", file_name[i], 0
}
