#include <block/sched.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <tests.h>

#define EXT2_INIT                                                              \
    if (g_root_node->fs_type != FS_EXT2) {                                     \
        ADD_MESSAGE("the mounted root is not ext2");                           \
        SET_SKIP;                                                              \
        return;                                                                \
    }                                                                          \
    struct vfs_node *root = g_root_node;

static void flush() {
    struct ext2_fs *fs = g_root_node->fs_data;
    struct generic_disk *d = fs->drive;

    bio_sched_dispatch_all(d);
}

REGISTER_TEST(ext2_stat_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    enum errno e = root->ops->create(root, "ext2_stat_test", VFS_MODE_FILE);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "ext2_stat_test");
    TEST_ASSERT(node != NULL);

    struct vfs_stat empty_stat = {0};
    struct vfs_stat stat_out = {0};
    node->ops->stat(node, &stat_out);

    /* this should return something */
    TEST_ASSERT(memcmp(&stat_out, &empty_stat, sizeof(struct vfs_stat)) != 0);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_rename_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;
    enum errno e = root->ops->create(root, "ext2_rename_test", VFS_MODE_FILE);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "ext2_rename_test");
    TEST_ASSERT(node != NULL);

    e = node->ops->rename(root, "ext2_rename_test", root, "ext2_rename_test2");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = root->ops->finddir(root, "ext2_rename_test");
    TEST_ASSERT(node == NULL);

    node = root->ops->finddir(root, "ext2_rename_test2");
    TEST_ASSERT(node != NULL);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_chmod_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    enum errno e = root->ops->create(root, "ext2_chmod_test", VFS_MODE_FILE);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "ext2_chmod_test");
    TEST_ASSERT(node != NULL);

    e = root->ops->chmod(node, (uint16_t) VFS_MODE_O_EXEC);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->mode & VFS_MODE_O_EXEC);

    node = root->ops->finddir(root, "ext2_chmod_test");
    TEST_ASSERT(node != NULL);
    TEST_ASSERT(node->mode & VFS_MODE_O_EXEC);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_symlink_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    enum errno e = root->ops->symlink(root, "/tmp", "ext2_symlink_test");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "ext2_symlink_test");
    TEST_ASSERT(node != NULL);

    char *buf = kzalloc(5);
    TEST_ASSERT(buf != NULL);

    e = node->ops->readlink(node, buf, 4);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(strcmp(buf, "/tmp") == 0);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_dir_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    enum errno e = root->ops->mkdir(root, "ext2_dir_test", VFS_MODE_DIR);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "ext2_dir_test");
    TEST_ASSERT(node != NULL);

    e = root->ops->rmdir(root, "ext2_dir_test");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_integration_test, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    EXT2_INIT;

    enum errno e =
        root->ops->create(root, "ext2_integration_test", VFS_MODE_FILE);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "ext2_integration_test");
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

    e = node->ops->unlink(root, "ext2_integration_test");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = root->ops->finddir(root, "ext2_integration_test");
    TEST_ASSERT(node == NULL);

    flush();

    SET_SUCCESS;
}
