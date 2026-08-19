#include "../test_internal.h"

#ifdef TEST_STRING
TEST_GROUP_DECLARE(string);

TEST_DECLARE_UNIT(kmp_strstr_patterns, .group = TEST_GROUP(string)) {
    /* Giving an empty needle returns start of haystack */
    const char *h1 = "abcdef";
    TEST_ASSERT(strstr(h1, "") == h1);

    TEST_ASSERT(strstr("short", "longer_needle") == NULL);

    TEST_ASSERT(strstr("hello", "hello") != NULL);

    /* KMP prefix backtracking */
    const char *h2 = "aabaabaabaax";
    const char *n2 = "aabaax";
    char *match2 = strstr(h2, n2);
    TEST_ASSERT(match2 != NULL);
    TEST_ASSERT(match2 == h2 + 6);

    /* trailing */
    const char *h3 = "aaaaab";
    const char *n3 = "aaab";
    char *match3 = strstr(h3, n3);
    TEST_ASSERT(match3 != NULL);
    TEST_ASSERT(match3 == h3 + 2);

    TEST_ASSERT(strstr("ababababa", "ababc") == NULL);

    return TEST_SUCCESS;
}
#endif
