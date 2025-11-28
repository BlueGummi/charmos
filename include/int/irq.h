/* @title: IRQs */
#pragma once
#include <smp/core.h>
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

struct isr_regs {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    // CPU-pushed interrupt frame:
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;

    // Only present on privilege-level change
    uint64_t rsp;
    uint64_t ss;
};

void irq_register(uint8_t vector, irq_handler_t handler, void *ctx);

static inline void irq_mark_self_in_interrupt(bool new) {
    smp_core()->in_interrupt = new;
}

static inline bool irq_in_interrupt(void) {
    return smp_core()->in_interrupt;
}

static inline bool irq_in_thread_context(void) {
    return !irq_in_interrupt();
}
