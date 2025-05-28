#include <stdint.h>

#define IDT_ENTRIES 256
#define MAX_IRQ 224
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

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

extern struct idt_entry idt[IDT_ENTRIES];

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_install();
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

static inline void trigger_interrupt(uint8_t code) {
    asm volatile("int %0" : : "i"(code));
}

#pragma once
