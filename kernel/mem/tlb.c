#include <acpi/lapic.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/tlb.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <thread/dpc.h>

/* NOTE: we avoid using a DPC for TLB shootdown due to the overhead of that */

#define TLB_SHOOTDOWN_INITIAL_SPIN 2000u /* tight CPU_relax loop first */
#define TLB_SHOOTDOWN_MAX_RETRIES 6u     /* total times we try sending IPIs */
#define TLB_SHOOTDOWN_BACKOFF_MULT                                             \
    4u /* multiply spin by this on each retry                                  \
        */
#define TLB_SHOOTDOWN_RESEND_PERIOD                                            \
    1u /* we will try to resend once per retry */

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

    tlb_shootdown_internal();
    lapic_write(LAPIC_REG_EOI, 0);
}

void tlb_shootdown(uintptr_t addr, bool synchronous) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    uint64_t gen = atomic_fetch_add(&global.next_tlb_gen, 1);
    size_t this_cpu = smp_core_id();

    size_t i;
    for_each_cpu_id(i) {
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

    for_each_cpu_id(i) {
        if (i == this_cpu)
            continue;
        struct tlb_shootdown_cpu *other = &global.shootdown_data[i];

        uint64_t target_gen = gen;
        size_t spin = TLB_SHOOTDOWN_INITIAL_SPIN;

        for (;;) {
            /* spin for 'spin' iterations checking the ack */
            size_t s;
            for (s = 0; s < spin; ++s) {
                if (atomic_load_explicit(&other->ack_gen,
                                         memory_order_acquire) >= target_gen)
                    break;

                cpu_relax();
            }

            if (atomic_load_explicit(&other->ack_gen, memory_order_acquire) >=
                target_gen)
                break; /* done for this CPU */

            /* Only resend if the target doesn't already have an IPI pending */
            bool already_pending =
                atomic_load_explicit(&other->ipi_pending, memory_order_acquire);
            if (!already_pending) {
                ipi_send(i, IRQ_TLB_SHOOTDOWN);
            }
            /* multiply spin, but clamp to some sane max to avoid huge loops */
            if ((uint64_t) spin * TLB_SHOOTDOWN_BACKOFF_MULT > (1u << 20))
                spin = (1u << 20);
            else
                spin *= TLB_SHOOTDOWN_BACKOFF_MULT;
        }
    }

out:

    irql_lower(irql);
}
