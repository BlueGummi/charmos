#include <stdalign.h>
#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct gdt_entry_tss {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

#define ACCESS_CODE_RING0 0x9A  // exec/read, ring 0
#define ACCESS_DATA_RING0 0x92  // read/write, ring 0
#define ACCESS_CODE_RING3 0xFA  // exec/read, ring 3
#define ACCESS_DATA_RING3 0xF2  // read/write, ring 3

#define GRAN_CODE 0xAF          // G=1, D/B=0, L=1, AVL=0
#define GRAN_DATA 0xAF          // G=1, D/B=0, L=0, AVL=0

void gdt_install();
// Kernel selectors (RPL = 0)
#define GDT_KERNEL_CODE 0x08 // index 1
#define GDT_KERNEL_DATA 0x10 // index 2

#define KERNEL_CS GDT_KERNEL_CODE
#define KERNEL_DS GDT_KERNEL_DATA

// User selectors (RPL = 3)
#define GDT_USER_CODE 0x28 // index 5 << 3
#define GDT_USER_DATA 0x30 // index 6 << 3

#define USER_CS (GDT_USER_CODE | 0x3) // == 0x2B
#define USER_DS (GDT_USER_DATA | 0x3) // == 0x33
#define USER_SS USER_DS

#pragma once
