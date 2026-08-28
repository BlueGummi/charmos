#include "tests/test_internal.h"

#ifdef TEST_STRING
TEST_GROUP_DECLARE(string);

TEST_DECLARE_UNIT(string, strstr_patterns) {
    /* Giving an empty needle returns start of haystack */
    const char *h1 = "abcdef";
    TEST_ASSERT_PTR_EQ(strstr(h1, ""), h1);

    TEST_ASSERT_NULL(strstr("short", "longer_needle"));

    TEST_ASSERT_NONNULL(strstr("hello", "hello"));

    /* KMP prefix backtracking */
    const char *h2 = "aabaabaabaax";
    const char *n2 = "aabaax";
    char *match2 = strstr(h2, n2);
    TEST_ASSERT_NONNULL(match2);
    TEST_ASSERT_PTR_EQ(match2, h2 + 6);

    /* trailing */
    const char *h3 = "aaaaab";
    const char *n3 = "aaab";
    char *match3 = strstr(h3, n3);
    TEST_ASSERT_NONNULL(match3);
    TEST_ASSERT_PTR_EQ(match3, h3 + 2);

    TEST_ASSERT_NULL(strstr("ababababa", "ababc"));

    return TEST_SUCCESS;
}
#endif
