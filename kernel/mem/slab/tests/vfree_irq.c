#include "mem/slab/tests/test_internal.h"
#include <acpi/lapic.h>
#include <irq/ipi.h>
#include <irq/irq.h>

#define VFREE_IRQ_TEST_SPIN_MASK 0x3f
static void **vfree_irq_allocs = NULL;
static size_t vfree_irq_total_allocs = 0;
static atomic_size_t vfree_irq_test_consumed = 0;

static enum irq_result vfree_irq_test_irq(void *ctx, uint8_t vector,
                                          struct irq_context *ictx) {
    cc_ignore(ctx, vector, ictx);
    size_t total = vfree_irq_total_allocs;
    int midrange = total / 10;
    int delta = (prng_next() % (midrange * 2)) - midrange;
    int possible = midrange + delta;
    if (possible < 1)
        possible = 1;

    if (possible + atomic_load(&vfree_irq_test_consumed) > total)
        possible = total - atomic_load(&vfree_irq_test_consumed);

    for (int i = 0; i < possible; i++) {
        size_t idx = atomic_inc(&vfree_irq_test_consumed);
        if (idx < total) {
            vfree_defer_irq(vfree_irq_allocs[idx]);
            int spins = prng_next() & VFREE_IRQ_TEST_SPIN_MASK;

            while (spins) {
                cpu_pause();
                spins--;
            }
        }
    }

    return IRQ_HANDLED;
}

TEST_DEFINE_INTEGRATION(slab, vfree_defer_irq, TEST_INTENSITY(256, 2048, 16384),
                        .min_cores = 4) {
    size_t total = ctx->intensity_val ? ctx->intensity_val : 2048;
    vfree_irq_total_allocs = total;
    vfree_irq_allocs = vnews(vfree_irq_allocs, total);
    TEST_ASSERT_NONNULL(vfree_irq_allocs);

    atomic_store(&vfree_irq_test_consumed, 0);

    irq_t irq = irq_alloc_entry();
    irq_register("vfree_defer_irq_test", irq, vfree_irq_test_irq, NULL,
                 IRQ_FLAG_NONE);
    irq_set_chip(irq, lapic_get_chip(), NULL);

    for (size_t i = 0; i < total; i++) {
        vfree_irq_allocs[i] = valloc(64);
        TEST_ASSERT_NONNULL(vfree_irq_allocs[i]);
    }

    while (atomic_load(&vfree_irq_test_consumed) < total) {
        ipi_send(3, irq);
        int spins = prng_next() & VFREE_IRQ_TEST_SPIN_MASK;

        while (spins) {
            cpu_pause();
            spins--;
        }
    }

    vfree(vfree_irq_allocs);
    vfree_irq_allocs = NULL;

    return TEST_SUCCESS;
}
