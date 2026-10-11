#include "mem/slab/tests/test_internal.h"

TEST_DEFINE_UNIT(slab, random_free_stress, TEST_INTENSITY(256, 2048, 32768),
                 .min_ram_mib = 8) {
    size_t n = ctx->intensity_val ? ctx->intensity_val : 2048;
    void **stress_alloc_free_ptrs = valloc(sizeof(void *) * n, ALLOC_ZERO);
    TEST_ASSERT_NONNULL(stress_alloc_free_ptrs);

    for (size_t i = 0; i < n; i++) {
        stress_alloc_free_ptrs[i] = valloc(64);
        TEST_ASSERT_NONNULL(stress_alloc_free_ptrs[i]);
    }

    for (size_t i = 0; i < n; i++) {
        uint64_t idx = prng_next() % n;
        if (stress_alloc_free_ptrs[idx]) {
            vfree(stress_alloc_free_ptrs[idx]);
            stress_alloc_free_ptrs[idx] = NULL;
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (stress_alloc_free_ptrs[i]) {
            vfree(stress_alloc_free_ptrs[i]);
        }
    }

    vfree(stress_alloc_free_ptrs);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(slab, bulk_alloc_free_stress, TEST_INTENSITY(256, 2048, 16384),
                 .min_ram_mib = 8) {
    size_t n = ctx->intensity_val ? ctx->intensity_val : 2048;
    void **mixed_stress_test_ptrs = valloc(sizeof(void *) * n);
    TEST_ASSERT_NONNULL(mixed_stress_test_ptrs);

    for (size_t i = 0; i < n; i++) {
        mixed_stress_test_ptrs[i] = valloc(128);
        TEST_ASSERT_NONNULL(mixed_stress_test_ptrs[i]);
    }

    for (size_t i = 0; i < n; i++) {
        vfree(mixed_stress_test_ptrs[i]);
    }

    vfree(mixed_stress_test_ptrs);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(slab, atomic_behavior_flags) {
    /* ALLOC_BEHAVIOR_ATOMIC should require nonpageable/nonmovable - allocator
       or sanitizers might coerce flags. This test ensures allocation doesn't
       return NULL for such a request. */
    uint16_t f = ALLOC_FLAG_NONPAGEABLE | ALLOC_FLAG_NONMOVABLE |
                 ALLOC_FLAG_NO_CACHE_ALIGN;
    void *p = valloc_new(
        256,
        (struct alloc_params){.flags = f, .behavior = ALLOC_BEHAVIOR_IRQ_SAFE});
    if (!p) {
        test_info("valloc_new failed for ATOMIC nonpageable request");
        return TEST_FAIL(NULL);
    }
    /* Do a quick write */
    volatile uint8_t *b = p;
    b[0] = 0x7E;
    if (b[0] != 0x7E) {
        test_info("atomic allocation memory check failed");
        vfree_new(p, ALLOC_BEHAVIOR_NORMAL);
        return TEST_FAIL(NULL);
    }
    vfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    test_info("behavior (ATOMIC) allocation passed");
    return TEST_SUCCESS;
}

TEST_DEFINE(slab, map_new) {
    return TEST_SUCCESS;
}
