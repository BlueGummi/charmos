#include "../test_internal.h"

#ifdef TEST_MM

TEST_DECLARE_SMOKE(page_alloc_demand_test, .group = TEST_GROUP(mem)) {
    void *ptr = page_alloc_demand(8, ALLOC_FLAGS_ZERO);
    memset(ptr, 67, PAGE_SIZE);
    test_info("successfully demand allocated and memsetted memory");
    return TEST_SUCCESS;
}
#endif
