#include "../test_internal.h"

#ifdef TEST_MEM

TEST_DECLARE_UNIT(slab, kmalloc_stress_alloc_free_test,
                  TEST_INTENSITY(256, 2048, 32768)) {
    ABORT_IF_RAM_LOW();

    size_t n = ctx->intensity_val ? ctx->intensity_val : 2048;
    void **stress_alloc_free_ptrs =
        kmalloc(sizeof(void *) * n, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(stress_alloc_free_ptrs);

    for (size_t i = 0; i < n; i++) {
        stress_alloc_free_ptrs[i] = kmalloc(64);
        TEST_ASSERT_NONNULL(stress_alloc_free_ptrs[i]);
    }

    for (size_t i = 0; i < n; i++) {
        uint64_t idx = prng_next() % n;
        if (stress_alloc_free_ptrs[idx]) {
            kfree(stress_alloc_free_ptrs[idx]);
            stress_alloc_free_ptrs[idx] = NULL;
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (stress_alloc_free_ptrs[i]) {
            kfree(stress_alloc_free_ptrs[i]);
        }
    }

    kfree(stress_alloc_free_ptrs);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(slab, kmalloc_mixed_stress_test,
                  TEST_INTENSITY(256, 2048, 16384)) {
    ABORT_IF_RAM_LOW();

    size_t n = ctx->intensity_val ? ctx->intensity_val : 2048;
    void **mixed_stress_test_ptrs = kmalloc(sizeof(void *) * n);
    TEST_ASSERT_NONNULL(mixed_stress_test_ptrs);

    for (size_t i = 0; i < n; i++) {
        mixed_stress_test_ptrs[i] = kmalloc(128);
        TEST_ASSERT_NONNULL(mixed_stress_test_ptrs[i]);
    }

    for (size_t i = 0; i < n; i++) {
        kfree(mixed_stress_test_ptrs[i]);
    }

    kfree(mixed_stress_test_ptrs);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(slab, kmalloc_new_behavior_test) {
    /* ALLOC_BEHAVIOR_ATOMIC should require nonpageable/nonmovable - allocator
       or sanitizers might coerce flags. This test ensures allocation doesn't
       return NULL for such a request. */
    uint16_t f = ALLOC_FLAG_NONPAGEABLE | ALLOC_FLAG_NONMOVABLE |
                 ALLOC_FLAG_NO_CACHE_ALIGN;
    void *p = kmalloc_new(256, f, ALLOC_BEHAVIOR_ATOMIC);
    if (!p) {
        test_info("kmalloc_new failed for ATOMIC nonpageable request");
        return TEST_FAIL(NULL);
    }
    /* Do a quick write */
    volatile uint8_t *b = p;
    b[0] = 0x7E;
    if (b[0] != 0x7E) {
        test_info("atomic allocation memory check failed");
        kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
        return TEST_FAIL(NULL);
    }
    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    test_info("behavior (ATOMIC) allocation passed");
    return TEST_SUCCESS;
}

TEST_DECLARE(slab, slab_map_new_test) {
    return TEST_SUCCESS;
}
#endif
