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

void gdt_install();
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

#define USER_DS 0x20

#define GDT_USER_CODE 0x23
#define GDT_USER_DATA 0x2B

#define USER_CS GDT_USER_CODE
#define USER_SS GDT_USER_DATA

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#pragma once
