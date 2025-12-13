/* @title: Interrupt Descriptor Table */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define IDT_ENTRIES 256

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_table {
    struct idt_entry entries[IDT_ENTRIES];
};

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void irq_init();
void irq_load();
void idt_set_gate(uint8_t num, uint16_t sel, uint8_t flags);
