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

    if (d == (void*)0x20)
        k_panic("Yo...\n");

    bio_sched_dispatch_all(d);
}

REGISTER_TEST(ext2_stat_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    FAIL_IF_FATAL(root->ops->create(root, "ext2_stat_test", VFS_MODE_FILE));

    struct vfs_node *node;
    struct vfs_dirent out;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_stat_test", &out));

    node = out.node;
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

    FAIL_IF_FATAL(root->ops->create(root, "ext2_rename_test", VFS_MODE_FILE));

    struct vfs_dirent out;
    struct vfs_node *node;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_rename_test", &out));

    node = out.node;
    TEST_ASSERT(node != NULL);

    FAIL_IF_FATAL(
        node->ops->rename(root, "ext2_rename_test", root, "ext2_rename_test2"));

    enum errno e = root->ops->finddir(root, "ext2_rename_test", &out);
    TEST_ASSERT(e == ERR_NO_ENT);

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_rename_test2", &out));

    node = out.node;
    TEST_ASSERT(node != NULL);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_chmod_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    FAIL_IF_FATAL(root->ops->create(root, "ext2_chmod_test", VFS_MODE_FILE));

    struct vfs_node *node;
    struct vfs_dirent ent;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_chmod_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    FAIL_IF_FATAL(root->ops->chmod(node, (uint16_t) VFS_MODE_O_EXEC));
    TEST_ASSERT(node->mode & VFS_MODE_O_EXEC);

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_chmod_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);
    TEST_ASSERT(node->mode & VFS_MODE_O_EXEC);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_symlink_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    FAIL_IF_FATAL(root->ops->symlink(root, "/tmp", "ext2_symlink_test"));

    struct vfs_dirent ent;
    struct vfs_node *node;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_symlink_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    char *buf = kzalloc(5);
    TEST_ASSERT(buf != NULL);

    FAIL_IF_FATAL(node->ops->readlink(node, buf, 4));

    TEST_ASSERT(strcmp(buf, "/tmp") == 0);

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_dir_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;

    FAIL_IF_FATAL(root->ops->mkdir(root, "ext2_dir_test", VFS_MODE_DIR));

    struct vfs_dirent ent;
    struct vfs_node *node;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_dir_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    FAIL_IF_FATAL(root->ops->rmdir(root, "ext2_dir_test"));

    flush();
    SET_SUCCESS;
}

REGISTER_TEST(ext2_integration_test, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    EXT2_INIT;

    FAIL_IF_FATAL(
        root->ops->create(root, "ext2_integration_test", VFS_MODE_FILE));

    struct vfs_dirent ent;
    struct vfs_node *node;
    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_integration_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    FAIL_IF_FATAL(node->ops->write(node, lstr, len, 0));
    TEST_ASSERT(node->size == len);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);

    FAIL_IF_FATAL(node->ops->read(node, out_buf, len, 0));

    TEST_ASSERT(memcmp(out_buf, lstr, len) == 0);

    FAIL_IF_FATAL(node->ops->truncate(node, len / 2));

    memset(out_buf, 0, len);

    FAIL_IF_FATAL(node->ops->read(node, out_buf, len, 0));

    TEST_ASSERT(strlen(out_buf) == len / 2);

    FAIL_IF_FATAL(node->ops->unlink(root, "ext2_integration_test"));

    enum errno e = root->ops->finddir(root, "ext2_integration_test", &ent);
    TEST_ASSERT(e == ERR_NO_ENT);

    flush();

    SET_SUCCESS;
}
