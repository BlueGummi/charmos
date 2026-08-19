#include "../test_internal.h"
#include <acpi/lapic.h>
#include <irq/irq.h>

#ifdef TEST_MEM

#define KFREE_IRQ_TEST_SPIN_MASK 0x3f
static void **kfree_irq_allocs = NULL;
static size_t kfree_irq_total_allocs = 0;
static atomic_size_t kfree_irq_test_consumed = 0;

static enum irq_result kfree_irq_test_irq(void *ctx, uint8_t vector,
                                          struct irq_context *ictx) {
    (void) ctx;
    (void) vector;
    (void) ictx;
    size_t total = kfree_irq_total_allocs;
    int midrange = total / 10;
    int delta = (prng_next() % (midrange * 2)) - midrange;
    int possible = midrange + delta;
    if (possible < 1)
        possible = 1;

    if (possible + atomic_load(&kfree_irq_test_consumed) > total)
        possible = total - atomic_load(&kfree_irq_test_consumed);

    for (int i = 0; i < possible; i++) {
        size_t idx = atomic_fetch_add(&kfree_irq_test_consumed, 1);
        if (idx < total) {
            kfree_defer_irq(kfree_irq_allocs[idx]);
            int spins = prng_next() & KFREE_IRQ_TEST_SPIN_MASK;

            while (spins) {
                cpu_relax();
                spins--;
            }
        }
    }

    return IRQ_HANDLED;
}

TEST_DECLARE_INTEGRATION(kfree_defer_irq_test, .group = TEST_GROUP(slab),
                         TEST_INTENSITY(256, 2048, 16384)) {
    if (global.core_count < 4) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    size_t total = ctx->intensity_val ? ctx->intensity_val : 2048;
    kfree_irq_total_allocs = total;
    kfree_irq_allocs = kmalloc(sizeof(void *) * total);
    TEST_ASSERT(kfree_irq_allocs != NULL);

    atomic_store(&kfree_irq_test_consumed, 0);

    irq_t irq = irq_alloc_entry();
    irq_register("kfree_defer_irq_test", irq, kfree_irq_test_irq, NULL,
                 IRQ_FLAG_NONE);
    irq_set_chip(irq, lapic_get_chip(), NULL);

    for (size_t i = 0; i < total; i++) {
        kfree_irq_allocs[i] = kmalloc(64);
        TEST_ASSERT(kfree_irq_allocs[i] != NULL);
    }

    while (atomic_load(&kfree_irq_test_consumed) < total) {
        ipi_send(3, irq);
        int spins = prng_next() & KFREE_IRQ_TEST_SPIN_MASK;

        while (spins) {
            cpu_relax();
            spins--;
        }
    }

    kfree(kfree_irq_allocs);
    kfree_irq_allocs = NULL;

    return TEST_SUCCESS;
}
#endif
