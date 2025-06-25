#include <console/printf.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <string.h>
#include <tests.h>

REGISTER_TEST(ext2_withdisk_test, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    if (g_root_node->fs_type != FS_EXT2) {
        ADD_MESSAGE("the mounted root is not ext2");
        SET_SKIP;
        return;
    }
    struct vfs_node *root = g_root_node;

    enum errno e = root->ops->create(root, "banana", VFS_MODE_FILE);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "banana");
    TEST_ASSERT(node != NULL);

    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    e = node->ops->write(node, lstr, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->size == len);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(memcmp(out_buf, lstr, len) == 0);

    e = node->ops->truncate(node, len / 2);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    memset(out_buf, 0, len);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(strlen(out_buf) == len / 2);

    e = node->ops->unlink(root, "banana");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = root->ops->finddir(root, "banana");
    TEST_ASSERT(node == NULL);

    SET_SUCCESS;
}
