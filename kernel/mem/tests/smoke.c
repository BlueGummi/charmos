#include "test_internal.h"

#ifdef TEST_MEM
TEST_GROUP_DECLARE(mem, .intensity_desc = {
                            .curve = TEST_SCALE_PIECEWISE_LOG,
                            .unit = "iters",
                        });

TEST_DECLARE_SMOKE(pmm_alloc_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page();
    TEST_ASSERT(p);
    return TEST_SUCCESS;
}
TEST_DECLARE_SMOKE(vmm_map_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    uint64_t p = pmm_alloc_page();
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_bump(p, PAGE_SIZE, 0);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, PAGE_SIZE, VMM_FLAG_NONE);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr, VMM_FLAG_NONE) == (uint64_t) -1);
    return TEST_SUCCESS;
}
#endif
