#include <stdbool.h>
#include <stdint.h>
#pragma once

#define IDT_ENTRIES 256
#define MAX_IRQ 224

#define IRQ_DIV_BY_Z 0x0
#define IRQ_DEBUG 0x1
#define IRQ_BREAKPOINT 0x3
#define IRQ_DBF 0x8
#define IRQ_SSF 0xC
#define IRQ_GPF 0xD
#define IRQ_PAGE_FAULT 0xE
#define IRQ_TIMER 0x20
#define IRQ_SCHEDULER IRQ_TIMER
#define IRQ_TLB_SHOOTDOWN 0x22
#define IRQ_PANIC 0x23
#define IRQ_NOP 0x24 /* This is here so cores can bother each other */

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

typedef void (*isr_handler_t)(void *ctx, uint8_t vector, void *rsp);

struct isr_entry {
    isr_handler_t handler;
    void *ctx;
};

void idt_init();
void idt_load();
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

void idt_set_alloc(int entry, bool used);
int idt_alloc_entry(void);
void idt_free_entry(int entry);
bool idt_is_installed(int entry);
void isr_register(uint8_t vector, isr_handler_t handler, void *ctx);
