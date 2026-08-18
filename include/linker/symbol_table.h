/* @title: Kernel symbol table */
#pragma once
#include <stdint.h>

#define KERNEL_SYMS_MAGIC 0x534d5953u /* "SYMS" */

#define KERNEL_SYMS_RESERVE 2621440

struct kernel_syms_hdr {
    uint32_t magic;
    uint32_t count;
    uint32_t strtab_off; /* from the start of the header */
    uint32_t lines_off;  /* 0 when the build produced no line table */
};

/* Sorted by addr, so lookups can bisect */
struct kernel_sym {
    uint64_t addr;
    uint32_t name_off; /* from the start of the string table */
    uint32_t _pad;
};

#define KERNEL_LINES_MAGIC 0x454e494cu /* "LINE" */

/* Address -> file:line
 *
 * A fixed width entry per row would be really big, so rows are a delta stream
 * instead. For every row, how far the address moved, then change in file and in
 * line. Every field is uleb128, and the two that can go backwards are zigzagged
 * first, so the decoder doesn't need a sign bit, which is about 3 bytes a row
 */

struct kernel_lines_hdr {
    uint32_t magic;
    uint32_t count;     /* entries in the stream */
    uint64_t base_addr; /* the address the first delta is relative to */
    uint32_t stream_off;
    uint32_t stream_len;
    uint32_t files_off; /* NULL separated names, indexed by the file field */
    uint32_t files_len;
};

extern const char kernel_syms_blob[KERNEL_SYMS_RESERVE];
