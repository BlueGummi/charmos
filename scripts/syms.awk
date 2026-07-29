BEGIN {
    print "#include <stdint.h>";

    print "#ifndef SYMS_DEFINED";
    print "#define SYMS_DEFINED";
    print "#include <linker/symbol_table.h>";
    print "const struct sym syms[] = {";
}
/ [tT] / {
    # Recorded so the kernel can tell whether this table still describes it
    if ($3 == "__etext")
        etext = $1;

    print "\t{ 0x"$1", \""$3"\" },";
}
END {
    print "};\n";
    print "const uint64_t syms_len = sizeof(syms) / sizeof(syms[0]);";
    printf "const uint64_t syms_etext = 0x%s;\n", (etext == "" ? "0" : etext);
    print "#endif";
}

