/* @title: Kernel symbol table */
#pragma once
#include <stdint.h>

/* The table is not compiled into the kernel, it is stamped into a reserved
 * section after the link, so it always describes the very image it sits in.
 *
 * That is the whole reason for this format. A table built from a C file is an
 * input to the link that only exists once the link is done, which is a cycle
 * no build graph can express, and so the two drift apart. Patching after the
 * fact turns it back into a straight line.
 *
 * Nothing in here is a pointer: nothing relocates the blob at stamp time, so
 * every reference is an offset and the table is position independent */

#define KERNEL_SYMS_MAGIC 0x534d5953u /* "SYMS", little endian */

/* Bytes set aside in .kernel_syms. The stamp pads to exactly this, so the
 * section never changes size and no address moves because of it. Raise it if
 * the build starts saying the table does not fit.
 *
 * Keep it a plain literal: kernel/CMakeLists.txt reads the value straight out
 * of this line so the two can never disagree */
#define KERNEL_SYMS_RESERVE 327680

struct kernel_syms_hdr {
    uint32_t magic;
    uint32_t count;
    uint32_t strtab_off; /* from the start of the header */
    uint32_t lines_off;  /* ditto; 0 when the build produced no line table */
};

/* Sorted by addr, so lookups can bisect */
struct kernel_sym {
    uint64_t addr;
    uint32_t name_off; /* from the start of the string table */
    uint32_t _pad;
};

#define KERNEL_LINES_MAGIC 0x454e494cu /* "LINE", little endian */

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
