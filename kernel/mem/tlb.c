#include <acpi/lapic.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/tlb.h>
#include <sch/dpc.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdint.h>

static void tlb_shootdown_internal(void) {
    size_t cpu = smp_core_id();
    struct tlb_shootdown_cpu *c = &global.shootdown_data[cpu];
    atomic_store_explicit(&c->in_tlb_shootdown, true, memory_order_release);

    uint32_t tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
    for (;;) {
        uint32_t head = atomic_load_explicit(&c->head, memory_order_acquire);
        while (tail != head) {
            uintptr_t addr = atomic_load_explicit(
                &c->queue[tail & (TLB_QUEUE_SIZE - 1)], memory_order_acquire);
            if (addr)
                invlpg(addr);
            tail++;
        }

        atomic_store_explicit(&c->tail, tail, memory_order_release);

        if (atomic_load_explicit(&c->flush_all, memory_order_relaxed)) {
            tlb_flush();
            atomic_store_explicit(&c->flush_all, 0, memory_order_relaxed);
        }

        uint32_t new_head =
            atomic_load_explicit(&c->head, memory_order_acquire);
        if (new_head == tail)
            break;
    }

    atomic_store_explicit(
        &c->ack_gen,
        atomic_load_explicit(&global.next_tlb_gen, memory_order_acquire),
        memory_order_release);

    atomic_store_explicit(&c->ipi_pending, false, memory_order_release);
    atomic_store_explicit(&c->in_tlb_shootdown, false, memory_order_release);
}

void tlb_shootdown_isr(void *ctx, uint8_t irq, void *rsp) {
    (void) ctx;
    (void) irq;
    (void) rsp;

    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES) {
        tlb_shootdown_internal();
    } else {
        dpc_enqueue_local(smp_core()->tlb_shootdown_dpc);
    }
    lapic_write(LAPIC_REG_EOI, 0);
}

void tlb_dpc_func(void *ctx) {
    tlb_shootdown_internal();
}

void tlb_shootdown(uintptr_t addr, bool synchronous) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    uint64_t gen = atomic_fetch_add(&global.next_tlb_gen, 1);
    size_t this_cpu = smp_core_id();

    for (size_t i = 0; i < global.core_count; i++) {
        if (i == this_cpu) {
            invlpg(addr);
            continue;
        }

        struct tlb_shootdown_cpu *t = &global.shootdown_data[i];

        /* try reserve slot */
        uint32_t slot =
            atomic_fetch_add_explicit(&t->head, 1, memory_order_acq_rel);
        bool full = false;
        if ((slot - atomic_load_explicit(&t->tail, memory_order_acquire)) >=
            TLB_QUEUE_SIZE) {
            /* full */
            full = true;
            atomic_store_explicit(&t->flush_all, 1, memory_order_release);
        } else {
            /* write address into slot */
            atomic_store_explicit(&t->queue[slot & (TLB_QUEUE_SIZE - 1)],
                                  (uintptr_t) addr, memory_order_release);
        }

        bool old =
            atomic_exchange_explicit(&t->ipi_pending, 1, memory_order_acq_rel);

        if (!old || full) {
            ipi_send(i, IRQ_TLB_SHOOTDOWN);
        }
    }

    /* wait if needed */
    if (!synchronous)
        goto out;

    for (size_t i = 0; i < global.core_count; i++) {
        if (i == this_cpu)
            continue;
        struct tlb_shootdown_cpu *other = &global.shootdown_data[i];
        while (atomic_load_explicit(&other->ack_gen, memory_order_acquire) <
               gen) {
            if (atomic_load_explicit(&other->in_tlb_shootdown,
                                     memory_order_acquire)) {
                cpu_relax();
            } else {
                ipi_send(i, IRQ_TLB_SHOOTDOWN);
            }
        }
    }

out:

    irql_lower(irql);
}

void tlb_init(void) {
    for (size_t i = 0; i < global.core_count; i++) {
        global.cores[i]->tlb_shootdown_dpc = dpc_create(tlb_dpc_func, NULL);
    }
}
