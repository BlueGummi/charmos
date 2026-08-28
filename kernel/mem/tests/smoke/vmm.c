#include "../test_internal.h"

#ifdef TEST_MEM

TEST_DECLARE_SMOKE(mem, vmm_map_test) {
    paddr_t p = pmm_alloc_page();
    TEST_ASSERT(p);

    void *va = vmm_map_bump(p, PAGE_SIZE, 0);
    TEST_ASSERT_NONNULL(va);

    *(volatile uint64_t *) va = 0xdeadbeef;
    TEST_ASSERT_EQ(*(volatile uint64_t *) va, 0xdeadbeef);

    return TEST_SUCCESS;
}
#endif
