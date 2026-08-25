#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <dbg.h>
#include <global.h>
#include <irq/exception_sync_cb.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <mem/page_fault.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <smp/smp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sync/rcu.h>
#include <thread/apc.h>
#include <thread/thread.h>
#include <time/timer.h>

/* Lock is only used for allocation/free and registering */
static struct spinlock irq_table_lock = SPINLOCK_INIT;
static struct irq_desc irq_table[IDT_ENTRIES] = {0};
static struct exception_sync_cb exception_cbs[IRQ_EXCEPTION_COUNT] = {0};
static struct idt_table idts = {0};
static struct idt_ptr idtps = {0};

#include "fault_isrs.h"
#include "isr_stubs.h"
#include "isr_vectors_array.h"

static void irq_execute_vector_handlers(irq_t vector,
                                        struct irq_context *irq_ctx) {
    struct irq_desc *desc = &irq_table[vector];
    if (!desc->present || list_empty(&irq_table[vector].actions))
        panic("Unhandled ISR vector: %u", vector);

    bool handled = false;
    struct list_head *lh;
    list_for_each(lh, &desc->actions) {
        struct irq_action *act = container_of(lh, struct irq_action, list);
        if (act->handler(act->data, vector, irq_ctx) == IRQ_HANDLED) {
            handled = true;
            break;
        }
    }

    if (handled && desc->chip && desc->chip->eoi)
        desc->chip->eoi(desc);
}

/* TODO: Someday we will need to handle nmi_depth > 1 and do a fancy asm
 * trampoline to handle cases where IRQs fire in NMIs due to the IRET issue */
void isr_nmi_entry(struct irq_context *irq_ctx) {
    /* vector == IRQ_NMI, of course */
    kassert(!irq_mark_self_in_nmi(true));

    irq_execute_vector_handlers(IRQ_NMI, irq_ctx);

    kassert(irq_mark_self_in_nmi(false) == 1);
}

void isr_standard_entry(irq_t vector, struct irq_context *irq_ctx) {
    irq_mark_self_in_interrupt(true);

    enum irql old = irql_raise(IRQL_HIGH_LEVEL);

    /* This can never happen, if so, somehow an IRQ fired at IRQL_HIGH_LEVEL,
     * besides which is not possible UNLESS it's the NMI */
    if (vector != IRQ_NMI)
        kassert(old != IRQL_HIGH_LEVEL);

    bool is_exception = irq_vector_is_exception(vector);
    uint8_t scratch_buf[EXCEPTION_SYNC_CB_SCRATCH_BUFFER_SIZE] = {0};

    if (vector != IRQ_NMI)
        kassert(smp_core()->irq_entered_irql == IRQL_NONE, "Potential race");

    smp_core()->irq_entered_irql = old;
    smp_core()->irq_stack_scratch_buf = scratch_buf;

    irq_execute_vector_handlers(vector, irq_ctx);

    smp_core()->irq_entered_irql = IRQL_NONE;
    smp_core()->irq_stack_scratch_buf = NULL;

    /* Here's an odd bit: we'll have very different
     * behavior if we came from an exception. Namely,
     * irql_lower will NOT `sti` if irq_in_interrupt()
     *
     * Thus, if we are in an exception, we FIRST
     * irq_mark_self_in_interrupt(false), and the we
     * irql_lower(old)
     *
     * This results in interrupts being enabled
     * when we come out of irql_lower(), and the
     * reason why this only applies to exceptions
     * is because they are synchronous, and more
     * importantly, have an exception_sync_cb that
     * can run, but only from this context in which we
     * are at our old pre-exception IRQL and have
     * interrupts enabled */
    if (is_exception) {
        irq_mark_self_in_interrupt(false);
        irql_lower(old);

        struct exception_sync_cb *escb = &exception_cbs[vector];
        if (escb->fn)
            escb->fn(escb, irq_ctx, scratch_buf);

    } else {
        irql_lower(old);
        irq_mark_self_in_interrupt(false);
    }

    /* This function just raises and lowers the IRQL, because
     * now we're irq_mark_self_in_interrupt == false */
    if (scheduler_mark_self_needs_run_dpcs(false))
        dpc_run_dpcs_from_irq();

    /* in reschedule, don't check if we need to preempt */
    if (scheduler_self_in_resched())
        return;

    if (!scheduler_preemption_disabled() &&
        scheduler_mark_self_needs_resched(false)) {
        struct thread *curr = thread_get_current();
        if (curr)
            curr->preemptions++;

        kassert(old != IRQL_DISPATCH_LEVEL);
        scheduler_yield();
    }
}

void isr_common_entry(irq_t vector, struct irq_context *irq_ctx) {
    if (vector != IRQ_NMI) {
        isr_standard_entry(vector, irq_ctx);
    } else {
        isr_nmi_entry(irq_ctx);
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
        panic("need to be shared to have many, registered by %s", me->name);

    struct irq_action *act =
        kmalloc_or_die(sizeof(struct irq_action), ALLOC_FLAGS_ZERO);

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

    if (irq_table[vec].chip && chip)
        panic("IRQ chip %u exists", vec);

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

    /* TODO: maybe don't hardcode this (?) */
    if (num == IRQ_NMI || num == IRQ_DBF || num == IRQ_PAGE_FAULT) {
        idt[num].ist = 1;
    } else {
        idt[num].ist = 0;
    }

    /* debug */
    idt[num].ist = 0;

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
    desc->name = NULL;
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
        kfree(iter);
    }

    irq_desc_clear(desc);

    spin_unlock(&irq_table_lock, irql);
}

void irq_disable(irq_t irq) {
    struct irq_desc *desc = &irq_table[irq];
    desc->enabled = false;
    if (desc->chip && desc->chip->mask)
        desc->chip->mask(desc);
}

void irq_enable(irq_t irq) {
    struct irq_desc *desc = &irq_table[irq];
    desc->enabled = true;
    if (desc->chip && desc->chip->unmask)
        desc->chip->unmask(desc);
}

static void exception_sync_cbs_init() {
    struct exception_sync_cb *esc;
    linker_section_for_each_object(esc, exception_sync_cbs) {
        kassert(exception_cbs[esc->vector].vector == 0);
        exception_cbs[esc->vector] = *esc;
    }
}

void irq_init() {
    for (size_t i = 0; i < IDT_ENTRIES; i++) {
        struct irq_desc *desc = &irq_table[i];
        alloc_or_die(cpu_mask_init(&desc->masked_cpus, global.core_count));
        alloc_or_die(cpu_mask_init(&desc->affinity, global.core_count));

        desc->vector = i;
        irq_desc_clear(desc);

        idt_set_gate(i, 0x08, 0x8e);
    }

    exception_sync_cbs_init();

    irq_register("division_by_zero", IRQ_DIV_BY_Z, divbyz_handler, NULL,
                 IRQ_FLAG_NONE);
    irq_register("debug", IRQ_DEBUG, debug_handler, NULL, IRQ_FLAG_NONE);
    irq_register("breakpoint", IRQ_BREAKPOINT, breakpoint_handler, NULL,
                 IRQ_FLAG_NONE);

    irq_register("ssf", IRQ_SSF, ss_handler, NULL, IRQ_FLAG_NONE);

    irq_register("gpf", IRQ_GPF, gpf_handler, NULL, IRQ_FLAG_NONE);
    irq_register("double_fault", IRQ_DBF, double_fault_handler, NULL,
                 IRQ_FLAG_NONE);
    irq_register("page_fault", IRQ_PAGE_FAULT, page_fault_isr, NULL,
                 IRQ_FLAG_NONE);

    irq_register("timer", IRQ_TIMER, timer_isr, NULL, IRQ_FLAG_NONE);
    irq_set_chip(IRQ_TIMER, lapic_get_chip(), NULL);

    /* NOTE: Ordering MATTERS here. We have to register this first, because
     * this means that the panic check fires *before* any other NMI check,
     * which is probably one of the biggest debug correctness parts
     * of this interrupt subsystem. This MUST stay here, or its registration
     * must guarantee that it is at the head of the NMI ISR list */
    irq_register("nmi", IRQ_NMI, panic_nmi_isr, NULL, IRQ_FLAG_SHARED);

    /* HACK: secondary names are not added onto additional registrations,
     * they just disappear. It's not at all important for correctness,
     * and there are no consumers of name anyways, but it's something we'll
     * need to implement some time down the line */
    irq_register("hardware_nmi", IRQ_NMI, hw_error_nmi_isr, NULL,
                 IRQ_FLAG_SHARED);

    irq_register("tlb_shootdown", IRQ_TLB_SHOOTDOWN, tlb_shootdown_isr, NULL,
                 IRQ_FLAG_NONE);
    irq_set_chip(IRQ_TLB_SHOOTDOWN, lapic_get_chip(), NULL);

    irq_register("nop", IRQ_NOP, nop_handler, NULL, IRQ_FLAG_NONE);
    irq_register("dpc", IRQ_DPC, dpc_handler, NULL, IRQ_FLAG_NONE);
    irq_set_chip(IRQ_DPC, lapic_get_chip(), NULL);

    idt_set_gate(0x80, 0x2b, 0xee);
    irq_load();
}
