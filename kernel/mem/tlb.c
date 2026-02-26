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

struct spinlock tlb_shootdown_lock = SPINLOCK_INIT;

static void tlb_shootdown_internal(void) {
    size_t cpu = smp_core_id();
    struct tlb_shootdown_cpu *c = &global.shootdown_data[cpu];

    uint64_t done = atomic_load_explicit(&c->done_gen, memory_order_relaxed);

    while (true) {
        uint64_t req = atomic_load_explicit(&c->req_gen, memory_order_acquire);

        if (done >= req)
            break;

        for (;;) {
            uint32_t tail =
                atomic_load_explicit(&c->tail, memory_order_relaxed);
            uint32_t head =
                atomic_load_explicit(&c->head, memory_order_acquire);

            if (tail == head)
                break;

            while (tail != head) {
                uintptr_t addr =
                    atomic_load_explicit(&c->queue[tail & (TLB_QUEUE_SIZE - 1)],
                                         memory_order_acquire);

                if (addr)
                    invlpg(addr);

                tail++;
            }

            atomic_store_explicit(&c->tail, tail, memory_order_release);
        }

        if (atomic_exchange_explicit(&c->flush_all, false,
                                     memory_order_acq_rel)) {
            tlb_flush();
            uint32_t h = atomic_load_explicit(&c->head, memory_order_acquire);
            atomic_store_explicit(&c->tail, h, memory_order_release);
        }

        done++;
        atomic_store_explicit(&c->done_gen, done, memory_order_release);
    }
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

    enum irql lirql = spin_lock(&tlb_shootdown_lock);

    uint64_t gen = atomic_fetch_add_explicit(&global.next_tlb_gen, 1,
                                             memory_order_relaxed) +
                   1;

    size_t this_cpu = smp_core_id();

    size_t i;
    for_each_cpu_id(i) {
        if (i == this_cpu) {
            invlpg(addr);
            continue;
        }

        struct tlb_shootdown_cpu *t = &global.shootdown_data[i];

        uint32_t head = atomic_load_explicit(&t->head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&t->tail, memory_order_acquire);

        if ((head - tail) >= TLB_QUEUE_SIZE) {
            atomic_store_explicit(&t->flush_all, true, memory_order_release);
        } else {
            atomic_store_explicit(&t->queue[head & (TLB_QUEUE_SIZE - 1)], addr,
                                  memory_order_release);
            atomic_store_explicit(&t->head, head + 1, memory_order_release);
        }

        atomic_store_explicit(&t->req_gen, gen, memory_order_release);

        if (atomic_load_explicit(&t->done_gen, memory_order_acquire) < gen) {
            ipi_send(i, IRQ_TLB_SHOOTDOWN);
        }
    }

    if (synchronous) {
        for_each_cpu_id(i) {
            if (i == this_cpu)
                continue;

            struct tlb_shootdown_cpu *o = &global.shootdown_data[i];

            int spins = 0;

            while (atomic_load_explicit(&o->done_gen, memory_order_acquire) <
                   gen) {
                if (spins < 1000) {
                    cpu_relax();
                    spins++;
                    continue;
                }

                ipi_send(i, IRQ_TLB_SHOOTDOWN);
            }
        }

        atomic_fetch_add_explicit(&global.pt_epoch, 1, memory_order_release);
    }

    spin_unlock(&tlb_shootdown_lock, lirql);
}
