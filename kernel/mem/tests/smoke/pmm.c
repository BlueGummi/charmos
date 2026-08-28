#include "../test_internal.h"

#ifdef TEST_MEM
TEST_GROUP_DECLARE(mem, .intensity_desc = {
                            .curve = SCALE_PIECEWISE_LOG,
                            .unit = "iters",
                        });

TEST_DECLARE_SMOKE(mem, pmm_alloc_test) {
    paddr_t p = pmm_alloc_page();
    TEST_ASSERT(p);
    pmm_free_page(p);
    return TEST_SUCCESS;
}
#endif
