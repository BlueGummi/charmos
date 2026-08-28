#include "mem/slab/tests/test_internal.h"

#ifdef TEST_MEM
TEST_GROUP_DECLARE(slab, .intensity_desc = {
                             .curve = SCALE_PIECEWISE_LOG,
                             .unit = "iters",
                         });

static char hooray[128] = {0};
TEST_DECLARE_SMOKE(slab, alloc_free_smoke) {

    void *p = kmalloc_new(67, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);

    time_ms_t ms = time_get_ms();
    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    ms = time_get_ms() - ms;

    snprintf(hooray, 128, "allocated %p and free took %u ms", p, ms);

    test_info(hooray);
    return TEST_SUCCESS;
}

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

static char a_msg[128];
TEST_DECLARE_SMOKE(slab, pattern_integrity) {

    void *p1 = kmalloc_new(1, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);
    void *p2 = kmalloc_new(64, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);
    void *p3 = kmalloc_new(4096, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);

    if (!p1 || !p2 || !p3) {
        test_info("kmalloc_new returned NULL for a valid request");
        return TEST_FAIL(NULL);
    }

    /* Write/read back small pattern to verify memory usable */
    memset(p1, 0xA5, 1);
    memset(p2, 0x5A, 64);
    memset(p3, 0xFF, 4096);

    if (((uint8_t *) p1)[0] != 0xA5 || ((uint8_t *) p2)[0] != 0x5A ||
        ((uint8_t *) p3)[0] != 0xFF) {
        test_info("Memory pattern check failed");
        return TEST_FAIL(NULL);
    }

    /* timed free to check that kfree_new returns quickly */
    time_ms_t start = time_get_ms();
    kfree_new(p1, ALLOC_BEHAVIOR_NORMAL);
    kfree_new(p2, ALLOC_BEHAVIOR_NORMAL);
    kfree_new(p3, ALLOC_BEHAVIOR_NORMAL);
    time_ms_t elapsed = time_get_ms() - start;

    snprintf(a_msg, sizeof(a_msg), "basic alloc/free OK (free took %u ms)",
             (unsigned) elapsed);
    test_info(a_msg);
    return TEST_SUCCESS;
}
#endif
