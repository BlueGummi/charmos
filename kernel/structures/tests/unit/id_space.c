#include "structures/tests/test_internal.h"

#ifdef TEST_ID_SPACE
TEST_GROUP_DECLARE(id_space);

TEST_DECLARE_UNIT(id_space, alloc_and_free_coalesce) {
    struct id_space *is = id_space_init(100);
    TEST_ASSERT_NONNULL(is);

    /* Allocate sequential IDs */
    uint64_t id1 = id_space_alloc(is);
    uint64_t id2 = id_space_alloc(is);
    uint64_t id3 = id_space_alloc(is);

    TEST_ASSERT_EQ(id1, 1);
    TEST_ASSERT_EQ(id2, 2);
    TEST_ASSERT_EQ(id3, 3);

    /* Free middle ID (id2) */
    id_space_free(is, id2);

    /* Next allocation should reuse id2 */
    uint64_t reused = id_space_alloc(is);
    TEST_ASSERT_EQ(reused, id2);

    /* Free all in non-sequential order and verify coalescing */
    id_space_free(is, id1);
    id_space_free(is, id3);
    id_space_free(is, reused);

    /* Allocate range */
    uint64_t range_start = id_space_alloc_range(is, 10);
    TEST_ASSERT_EQ(range_start, 1);

    id_space_free_range(is, range_start, 10);

    /* Verify after full free that ID 1 is available again */
    uint64_t id_again = id_space_alloc(is);
    TEST_ASSERT_EQ(id_again, 1);
    id_space_free(is, id_again);

    id_space_destroy(is);
    return TEST_SUCCESS;
}
#endif
