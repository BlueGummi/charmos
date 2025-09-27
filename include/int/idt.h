#include <stdbool.h>
#include <stdint.h>
#pragma once

#define IDT_ENTRIES 256
#define MAX_IRQ 224

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

void idt_init();
void idt_load();
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

void idt_set_alloc(int entry, bool used);
int idt_alloc_entry(void);
void idt_free_entry(int entry);
bool idt_is_installed(int entry);
