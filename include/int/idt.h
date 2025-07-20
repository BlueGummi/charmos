#include <stdbool.h>
#include <stdint.h>
#pragma once

#define IDT_ENTRIES 256
#define MAX_IRQ 224

#define DIV_BY_Z_ID 0x0
#define DEBUG_ID 0x1
#define BREAKPOINT_ID 0x3
#define DBF_ID 0x8
#define SSF_ID 0xC
#define GPF_ID 0xD
#define PAGE_FAULT_ID 0xE
#define TIMER_ID 0x20
#define SCHEDULER_ID TIMER_ID
#define KB_ID 0x21
#define TLB_SHOOTDOWN_ID 0x22

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

typedef void (*isr_handler_t)(void *ctx, uint8_t vector, void *rsp);

struct isr_entry {
    isr_handler_t handler;
    void *ctx;
};

void idt_install(uint64_t ind);
void idt_load(uint64_t ind);
void idt_alloc();
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
                  uint64_t ind);

void idt_set_alloc(int entry, uint64_t c, bool used);
int idt_alloc_entry(void);
int idt_alloc_entry_on_core(uint64_t core);
void idt_free_entry(int entry);
bool idt_is_installed(int entry);
void isr_register(uint8_t vector, isr_handler_t handler, void *ctx,
                  uint64_t core);
void lapic_send_ipi(uint8_t apic_id, uint8_t vector);
