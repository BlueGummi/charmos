/* @title: IRQs */
#pragma once
#include <smp/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/list.h>
#include <types/types.h>

#define IRQ_DIV_BY_Z 0x0
#define IRQ_DEBUG 0x1
#define IRQ_NMI 0x2
#define IRQ_BREAKPOINT 0x3
#define IRQ_DBF 0x8
#define IRQ_SSF 0xC
#define IRQ_GPF 0xD
#define IRQ_PAGE_FAULT 0xE
#define IRQ_TIMER 0x20
#define IRQ_SCHEDULER IRQ_TIMER
#define IRQ_TLB_SHOOTDOWN 0x22
#define IRQ_NOP 0x24

struct irq_context;
struct irq_desc;

enum irq_result {
    IRQ_NONE = 0,
    IRQ_HANDLED = 1,
};

typedef enum irq_result (*irq_handler_t)(void *ctx, uint8_t vector,
                                         struct irq_context *ictx);

enum irq_flags {
    IRQ_FLAG_SHARED = 1, /* IRQ is shared between things */
    IRQ_FLAG_LEVEL_TRIGGERED = 1 << 1,
    IRQ_FLAG_EDGE_TRIGGERED = 1 << 2,
    IRQ_FLAG_NONE = 0,
};

struct irq_action {
    irq_handler_t handler;
    void *data;
    struct list_head list;
};

struct irq_chip {
    const char *name;

    void (*mask)(struct irq_desc *);
    void (*unmask)(struct irq_desc *);
    void (*eoi)(struct irq_desc *);

    void (*set_affinity)(struct irq_desc *, struct cpu_mask *);
    int (*set_rate_limit)(struct irq_desc *, time_t interval);
};

struct irq_desc {
    uint8_t vector;
    enum irq_flags flags;

    const char *name;

    struct irq_chip *chip;
    void *chip_data;

    struct list_head actions;

    struct cpu_mask affinity;
    struct cpu_mask masked_cpus;

    bool present; /* Have we set handlers? */
    bool allocated; /* Has this been allocated? */
    bool enabled;
};

struct irq_context {
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

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;

    uint64_t rsp;
    uint64_t ss;
};

void irq_register(char *name, uint8_t vector, irq_handler_t handler, void *ctx,
                  enum irq_flags flags);
void irq_register_full(struct irq_desc *d);
void irq_set_chip(uint8_t vector, struct irq_chip *chip, void *data);

static inline void irq_mark_self_in_interrupt(bool new) {
    smp_core()->in_interrupt = new;
}

static inline bool irq_in_interrupt(void) {
    return smp_core()->in_interrupt;
}

static inline bool irq_in_thread_context(void) {
    return !irq_in_interrupt();
}

void ipi_send(uint32_t apic_id, uint8_t vector);
void nmi_send(uint32_t apic_id);

void irq_set_alloc(int32_t entry, bool used);
int32_t irq_alloc_entry(void);
void irq_free_entry(int32_t entry);
bool irq_is_installed(int32_t entry);
void irq_free_entry(int32_t entry);
void irq_disable(uint8_t irq);
void irq_enable(uint8_t irq);
