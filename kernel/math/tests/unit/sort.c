#include "math/tests/test_internal.h"

#ifdef TEST_SORT
TEST_GROUP_DECLARE(sort);

static int cmp_int(const void *a, const void *b) {
    int va = *(const int *) a;
    int vb = *(const int *) b;
    return (va > vb) - (va < vb);
}

TEST_DECLARE_UNIT(sort, heapsort_and_bsearch) {
    int arr[] = {42, 17, 93, 1, 56, 8, 23, 74, 31, 65};
    size_t n = sizeof(arr) / sizeof(arr[0]);

    int res = heapsort(arr, n, sizeof(int), cmp_int);
    TEST_ASSERT_EQ(res, 0);

    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_LE_S(arr[i - 1], arr[i]);
    }

    for (size_t i = 0; i < n; i++) {
        int key = arr[i];
        int *found = bsearch(&key, arr, n, sizeof(int), cmp_int);
        TEST_ASSERT_NONNULL(found);
        TEST_ASSERT_EQ(*found, key);
        TEST_ASSERT_EQ(found, &arr[i]);
    }

    int missing_low = 0;
    int missing_mid = 50;
    int missing_high = 1000;
    TEST_ASSERT_NULL(bsearch(&missing_low, arr, n, sizeof(int), cmp_int));
    TEST_ASSERT_NULL(bsearch(&missing_mid, arr, n, sizeof(int), cmp_int));
    TEST_ASSERT_NULL(bsearch(&missing_high, arr, n, sizeof(int), cmp_int));

    return TEST_SUCCESS;
}
#endif
