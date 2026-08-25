# Pull (address, line, file) rows out of `objdump --dwarf=decodedline`
#
# Rows look like:
#
#   kernel/acpi/dmar.c        93  0xffffffff80016700               x

function endswith(s, suf) {
    return length(s) >= length(suf) &&
           substr(s, length(s) - length(suf) + 1) == suf
}

function resolve_file(col,    f, i, hit, nhit, res) {
    if ((cur, col) in resolved)
        return resolved[cur, col]

    # Fits the column, so it came through whole
    if (col in full) {
        resolved[cur, col] = full[col]
        return full[col]
    }

    # Matches the current compilation unit
    if (cur != "" && endswith(cur, col)) {
        resolved[cur, col] = cur
        return cur
    }

    nhit = 0
    for (i = 0; i < n_unique; i++) {
        f = unique_files[i]
        if (endswith(f, col)) {
            hit = f
            nhit++
        }
    }

    if (nhit == 1) {
        resolved[cur, col] = hit
        return hit
    }

    res = col
    resolved[cur, col] = res
    return res
}

BEGIN {
    if (HIGH == "")
        HIGH = "ffffffff"
    cur = ""
    n_unique = 0
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
    if (!(cur in seen_file)) {
        seen_file[cur] = 1
        unique_files[n_unique++] = cur
    }
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
