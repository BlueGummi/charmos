#include <stdint.h>

#define IDT_ENTRIES 256
#define MAX_IRQ 224

#define DIV_BY_Z_ID 0x0
#define DEBUG_ID 0x1
#define BREAKPOINT_ID 0x3
#define DOUBLEFAULT_ID 0x8
#define SSF_ID 0xC
#define GPF_ID 0xD
#define PAGE_FAULT_ID 0xE
#define TIMER_ID 0x20
#define KB_ID 0x21

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

extern struct idt_table *idts;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_install(uint64_t ind);
void idt_load(uint64_t ind);
void idt_alloc(uint64_t size);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
                  uint64_t ind);

int idt_install_handler(uint8_t flags, void (*handler)(void), uint64_t core);
int idt_alloc_entry(void);
void idt_free_entry(int entry);
#pragma once
