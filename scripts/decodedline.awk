# Pull (address, line, file) rows out of `objdump --dwarf=decodedline`
#
# Rows look like:
#
#   kernel/acpi/dmar.c        93  0xffffffff80016700               x

function endswith(s, suf) {
    return length(s) >= length(suf) &&
           substr(s, length(s) - length(suf) + 1) == suf
}

function resolve_file(col,    f, hit, nhit) {
    if (col in resolved)
        return resolved[col]

    # Fits the column, so it came through whole
    if (col in full) {
        resolved[col] = full[col]
        return full[col]
    }

    nhit = 0
    for (f in full) {
        if (endswith(f, col)) {
            hit = full[f]
            nhit++
        }
    }

    if (nhit == 1) {
        resolved[col] = hit
        return hit
    }

    if (nhit > 1 && cur != "" && endswith(cur, col))
        return cur

    return col
}

BEGIN {
    if (HIGH == "")
        HIGH = "ffffffff"
    cur = ""
}

/:[ \t]*$/ && !/0x[0-9a-fA-F]/ {
    raw = $0
    sub(/:[ \t]*$/, "", raw)
    sub(/^CU:[ \t]*/, "", raw)

    cur = raw
    if (ROOT != "")
        sub("^" ROOT "/", "", cur)

    full[raw] = cur
    full[cur] = cur
    next
}

$3 ~ /^0x[0-9a-fA-F]+$/ && $2 ~ /^[0-9]+$/ {
    if ($2 + 0 == 0)
        next

    h = tolower(substr($3, 3))
    while (length(h) < 16)
        h = "0" h

    if (substr(h, 1, 8) != HIGH)
        next

    print h "\t" $2 "\t" resolve_file($1)
}
