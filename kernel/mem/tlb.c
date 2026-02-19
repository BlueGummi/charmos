#include <acpi/lapic.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/tlb.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <thread/dpc.h>

/* TODO: I have applied bandaid fixes to make this "work", however
 * scalability and performance will perform badly here, please fix */

#define TLB_SHOOTDOWN_INITIAL_SPIN 2000u /* tight CPU_relax loop first */
#define TLB_SHOOTDOWN_MAX_RETRIES 6u     /* total times we try sending IPIs */
#define TLB_SHOOTDOWN_BACKOFF_MULT                                             \
    4u /* multiply spin by this on each retry                                  \
        */
#define TLB_SHOOTDOWN_RESEND_PERIOD                                            \
    1u /* we will try to resend once per retry */

struct spinlock tlb_shootdown_lock = SPINLOCK_INIT;

static void tlb_shootdown_internal(void) {
    size_t cpu = smp_core_id();
    struct tlb_shootdown_cpu *c = &global.shootdown_data[cpu];

    atomic_store_explicit(&c->in_tlb_shootdown, true, memory_order_release);

    uint32_t tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
    while (true) {
        uint32_t head = atomic_load_explicit(&c->head, memory_order_acquire);

        while (tail != head) {
            uintptr_t addr = atomic_load_explicit(
                &c->queue[tail & (TLB_QUEUE_SIZE - 1)], memory_order_acquire);
            if (addr)
                invlpg(addr);
            tail++;
        }

        atomic_store_explicit(&c->tail, tail, memory_order_release);

        if (atomic_load_explicit(&c->flush_all, memory_order_acquire)) {
            tlb_flush();
            atomic_store_explicit(&c->flush_all, false, memory_order_release);

            uint32_t h = atomic_load_explicit(&c->head, memory_order_acquire);
            atomic_store_explicit(&c->tail, h, memory_order_release);
        }

        uint32_t new_head =
            atomic_load_explicit(&c->head, memory_order_acquire);
        if (new_head == tail)
            break;
    }

    uint64_t gen = atomic_load_explicit(&c->target_gen, memory_order_acquire);
    atomic_store_explicit(&c->ack_gen, gen, memory_order_release);

    atomic_store_explicit(&c->ipi_pending, false, memory_order_release);
    atomic_store_explicit(&c->in_tlb_shootdown, false, memory_order_release);
}

enum irq_result tlb_shootdown_isr(void *ctx, uint8_t irq,
                                  struct irq_context *rsp) {
    (void) ctx;
    (void) irq;
    (void) rsp;

    tlb_shootdown_internal();
    return IRQ_HANDLED;
}

void tlb_shootdown(uintptr_t addr, bool synchronous) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    enum irql lirql = spin_lock(&tlb_shootdown_lock);

    uint64_t gen = atomic_fetch_add(&global.next_tlb_gen, 1);

    size_t this_cpu = smp_core_id();
    size_t i;

    for_each_cpu_id(i) {
        if (i == this_cpu) {
            invlpg(addr);
            continue;
        }

        struct tlb_shootdown_cpu *t = &global.shootdown_data[i];

        atomic_store_explicit(&t->target_gen, gen, memory_order_release);

        uint32_t head = atomic_load_explicit(&t->head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&t->tail, memory_order_acquire);

        if ((head - tail) >= TLB_QUEUE_SIZE) {
            atomic_store_explicit(&t->flush_all, true, memory_order_release);
        } else {
            atomic_store_explicit(&t->queue[head & (TLB_QUEUE_SIZE - 1)], addr,
                                  memory_order_release);
            atomic_store_explicit(&t->head, head + 1, memory_order_release);
        }

        bool old_pending = atomic_exchange_explicit(&t->ipi_pending, true,
                                                    memory_order_acq_rel);
        if (!old_pending ||
            atomic_load_explicit(&t->flush_all, memory_order_acquire)) {
            ipi_send(i, IRQ_TLB_SHOOTDOWN);
        }
    }

    if (synchronous) {
        for_each_cpu_id(i) {
            if (i == this_cpu)
                continue;
            struct tlb_shootdown_cpu *other = &global.shootdown_data[i];
            uint64_t target_gen = gen;
            size_t spin = TLB_SHOOTDOWN_INITIAL_SPIN;

            while (atomic_load_explicit(&other->ack_gen, memory_order_acquire) <
                   target_gen) {
                for (size_t s = 0; s < spin; ++s) {
                    if (atomic_load_explicit(&other->ack_gen,
                                             memory_order_acquire) >=
                        target_gen)
                        break;
                    cpu_relax();
                }

                if (atomic_load_explicit(&other->ack_gen,
                                         memory_order_acquire) < target_gen) {
                    ipi_send(i, IRQ_TLB_SHOOTDOWN);
                }

                if ((uint64_t) spin * TLB_SHOOTDOWN_BACKOFF_MULT > (1u << 20))
                    spin = (1u << 20);
                else
                    spin *= TLB_SHOOTDOWN_BACKOFF_MULT;
            }
        }
    }

    spin_unlock(&tlb_shootdown_lock, lirql);
    irql_lower(irql);
}
