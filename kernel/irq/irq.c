#include <acpi/lapic.h>
#include <asm.h>
#include <charmos.h>
#include <compiler.h>
#include <console/printf.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <smp/smp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/rcu.h>
#include <thread/apc.h>

extern enum irq_result page_fault_handler(void *context, uint8_t vector,
                                          struct irq_context *rsp);

/* Lock is only used for allocation/free and registering */
static struct spinlock irq_table_lock = SPINLOCK_INIT;
static struct irq_desc irq_table[IDT_ENTRIES] = {0};
static struct idt_table idts = {0};
static struct idt_ptr idtps = {0};

#include "fault_isrs.h"
#include "isr_stubs.h"
#include "isr_vectors_array.h"

void isr_common_entry(uint8_t vector, struct irq_context *rsp) {
    irq_mark_self_in_interrupt(true);

    enum irql old = irql_raise(IRQL_HIGH_LEVEL);

    struct irq_desc *desc = &irq_table[vector];
    if (!desc->present || list_empty(&irq_table[vector].actions))
        k_panic("Unhandled ISR vector: %u\n", vector);

    bool handled = false;
    struct list_head *lh;
    list_for_each(lh, &desc->actions) {
        struct irq_action *act = container_of(lh, struct irq_action, list);
        if (act->handler(act->data, vector, rsp) == IRQ_HANDLED) {
            handled = true;
            break;
        }
    }

    if (handled && desc->chip && desc->chip->eoi)
        desc->chip->eoi(desc);

    irql_lower(old);
    irq_mark_self_in_interrupt(false);

    /* in reschedule, don't check if we need to preempt */
    if (scheduler_self_in_resched())
        return;

    if (!scheduler_preemption_disabled() &&
        scheduler_mark_self_needs_resched(false)) {
        struct thread *curr = scheduler_get_current_thread();
        if (curr)
            curr->preemptions++;

        kassert(old != IRQL_DISPATCH_LEVEL);
        scheduler_yield();
    }
}

void irq_register(char *name, uint8_t vector, irq_handler_t handler, void *ctx,
                  enum irq_flags flags) {
    enum irql irql = spin_lock(&irq_table_lock);
    struct irq_desc *me = &irq_table[vector];

    bool was = me->present;
    me->present = true;
    me->allocated = true;
    me->enabled = true;

    if (was && !(flags & IRQ_FLAG_SHARED))
        k_panic("need to be shared to have many, registered by %s\n", me->name);

    struct irq_action *act =
        kzalloc(sizeof(struct irq_action), ALLOC_PARAMS_DEFAULT);

    if (!act)
        k_panic("OOM - TODO: make this dynamic\n");

    act->handler = handler;
    INIT_LIST_HEAD(&act->list);
    act->data = ctx;

    list_add_tail(&act->list, &me->actions);
    if (!me->name)
        me->name = name;

    me->flags = flags;

    spin_unlock(&irq_table_lock, irql);
}

void irq_set_chip(uint8_t vec, struct irq_chip *chip, void *data) {
    enum irql irql = spin_lock(&irq_table_lock);

    if (irq_table[vec].chip)
        k_panic("IRQ chip exists\n");

    irq_table[vec].chip = chip;
    irq_table[vec].chip_data = data;

    spin_unlock(&irq_table_lock, irql);
}

void idt_set_gate(uint8_t num, uint16_t sel, uint8_t flags) {
    struct idt_entry *idt = idts.entries;

    uint64_t base = (uint64_t) isr_vectors[num];

    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;

    /* TODO: maybe don't hardcode this */
    if (num == IRQ_NMI || num == IRQ_DBF) {
        idt[num].ist = 1;
    } else {
        idt[num].ist = 0;
    }

    idt[num].flags = flags;
    idt[num].reserved = 0;
}

void irq_load(void) {
    idtps.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtps.base = (uint64_t) &idts;
    asm volatile("lidt %0" : : "m"(idtps));
}

int32_t irq_alloc_entry() {
    enum irql irql = spin_lock(&irq_table_lock);
    for (int32_t i = 32; i < IDT_ENTRIES; i++) {
        if (!irq_table[i].allocated) {
            irq_table[i].allocated = true;
            spin_unlock(&irq_table_lock, irql);
            return i;
        }
    }
    spin_unlock(&irq_table_lock, irql);
    return -1;
}

static void irq_desc_clear(struct irq_desc *desc) {
    cpu_mask_set_all(&desc->masked_cpus);
    cpu_mask_set_all(&desc->affinity);
    INIT_LIST_HEAD(&desc->actions);
    desc->present = false;
    desc->allocated = false;
    desc->enabled = false;
    desc->flags = 0;
    desc->name = "none";
}

void irq_free_entry(int32_t entry) {
    if (entry < 32 || entry >= IDT_ENTRIES)
        return;

    enum irql irql = spin_lock(&irq_table_lock);

    struct irq_desc *desc = &irq_table[entry];
    desc->allocated = false;
    desc->present = false;

    struct irq_action *iter, *tmp;
    list_for_each_entry_safe(iter, tmp, &desc->actions, list) {
        list_del_init(&iter->list);
        kfree(iter, FREE_PARAMS_DEFAULT);
    }

    irq_desc_clear(desc);

    spin_unlock(&irq_table_lock, irql);
}

void irq_disable(uint8_t irq) {
    struct irq_desc *desc = &irq_table[irq];
    desc->enabled = false;
    if (desc->chip && desc->chip->mask)
        desc->chip->mask(desc);
}

void irq_enable(uint8_t irq) {
    struct irq_desc *desc = &irq_table[irq];
    desc->enabled = true;
    if (desc->chip && desc->chip->unmask)
        desc->chip->unmask(desc);
}

void irq_init() {
    for (size_t i = 0; i < IDT_ENTRIES; i++) {
        struct irq_desc *desc = &irq_table[i];
        if (!cpu_mask_init(&desc->masked_cpus, global.core_count))
            k_panic("OOM\n");

        if (!cpu_mask_init(&desc->affinity, global.core_count))
            k_panic("OOM\n");

        desc->vector = i;
        irq_desc_clear(desc);
        idt_set_gate(i, 0x08, 0x8e);
    }

    irq_register("division_by_zero", IRQ_DIV_BY_Z, divbyz_handler, NULL,
                 IRQ_FLAG_NONE);
    irq_register("debug", IRQ_DEBUG, debug_handler, NULL, IRQ_FLAG_NONE);
    irq_register("breakpoint", IRQ_BREAKPOINT, breakpoint_handler, NULL,
                 IRQ_FLAG_NONE);

    irq_register("ssf", IRQ_SSF, ss_handler, NULL, IRQ_FLAG_NONE);
    irq_register("gpf", IRQ_GPF, gpf_handler, NULL, IRQ_FLAG_NONE);
    irq_register("double_fault", IRQ_DBF, double_fault_handler, NULL,
                 IRQ_FLAG_NONE);
    irq_register("page_fault", IRQ_PAGE_FAULT, page_fault_handler, NULL,
                 IRQ_FLAG_NONE);

    irq_register("timer", IRQ_TIMER, scheduler_timer_isr, NULL, IRQ_FLAG_NONE);
    irq_register("nmi", IRQ_NMI, nmi_isr, NULL, IRQ_FLAG_NONE);
    irq_register("tlb_shootdown", IRQ_TLB_SHOOTDOWN, tlb_shootdown_isr, NULL,
                 IRQ_FLAG_NONE);
    irq_register("nop", IRQ_NOP, nop_handler, NULL, IRQ_FLAG_NONE);
    idt_set_gate(0x80, 0x2b, 0xee);
    irq_load();
}
