#include "../test_internal.h"

#ifdef TEST_MEM

TEST_DECLARE_UNIT(tlb_shootdown_single_cpu_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page();
    paddr_t p2 = pmm_alloc_page();
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_bump(p1, PAGE_SIZE, 0);
    TEST_ASSERT(va);

    *(volatile uint64_t *) va = 0x11111111;

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_bump(p2, PAGE_SIZE, 0);

    tlb_shootdown((uintptr_t) va, true);

    *(volatile uint64_t *) va = 0x22222222;
    TEST_ASSERT(*(volatile uint64_t *) va == 0x22222222);

    return TEST_SUCCESS;
}
#endif
