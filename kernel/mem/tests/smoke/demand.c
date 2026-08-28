#include "../test_internal.h"

#ifdef TEST_MM

TEST_DECLARE_SMOKE(mem, page_alloc_demand_test) {
    void *ptr = page_alloc_demand(8, ALLOC_FLAGS_ZERO);
    memset(ptr, 67, PAGE_SIZE);
    test_info("successfully demand allocated and memsetted memory");
    return TEST_SUCCESS;
}
#endif
