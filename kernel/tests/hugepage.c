#include <mem/hugepage.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>

REGISTER_TEST(hugepage_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    void *ptr = hugepage_alloc_pages(1);
    TEST_ASSERT(ptr);
    ptr = hugepage_alloc_pages(1024);
    TEST_ASSERT(ptr);
    SET_SUCCESS;
}
