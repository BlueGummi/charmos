BEGIN {
    print "#include <stdint.h>";

    print "#ifndef SYMS_DEFINED";
    print "#define SYMS_DEFINED";
    print "#include <linker/symbol_table.h>";
    print "const struct sym syms[] = {";
}
/ [tT] / {
    print "\t{ 0x"$1", \""$3"\" },";
}
END {
    print "};\n";
    print "const uint64_t syms_len = sizeof(syms) / sizeof(syms[0]);";
    print "#endif";
}

