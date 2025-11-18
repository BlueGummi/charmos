#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*irq_handler_t)(void *ctx, uint8_t vector, void *rsp);

struct irq_entry {
    irq_handler_t handler;
    void *ctx;
};

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

struct irq_context {
    uint64_t rax, rbx, rcx, rdx, rbp, rdi, rsi, r8, r9, r10, r11, r12, r13, r14,
        r15, rip, cs, rflags;
};

bool irq_in_thread_context();
bool irq_in_interrupt();
void irq_mark_self_in_interrupt(bool new);
void irq_register(uint8_t vector, irq_handler_t handler, void *ctx);
