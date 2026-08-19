#include "../test_internal.h"

#ifdef TEST_EXT2
TEST_GROUP_DECLARE(ext2, .intensity_desc = {
                            .curve = TEST_SCALE_PIECEWISE_LOG,
                            .unit = "ops",
                        });

#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        test_info("the mounted root is not ext2");                             \
        return TEST_SKIP(TEST_SKIP_NONE);                                      \
    }                                                                          \
    struct vfs_node *root = global.root_node;

static void flush() {
    struct ext2_fs *fs = global.root_node->fs_data;
    struct block_device *d = fs->drive;

    bio_sched_dispatch_all(d);
}

TEST_DECLARE_INTEGRATION(ext2_stat_test, .group = TEST_GROUP(ext2),
                         TEST_INTENSITY(1, 1, 16)) {
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
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(ext2_rename_test, .group = TEST_GROUP(ext2),
                         TEST_INTENSITY(1, 1, 16)) {
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
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(ext2_chmod_test, .group = TEST_GROUP(ext2),
                         TEST_INTENSITY(1, 1, 16)) {
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
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(ext2_symlink_test, .group = TEST_GROUP(ext2),
                         TEST_INTENSITY(1, 1, 16)) {
    EXT2_INIT;

    FAIL_IF_FATAL(root->ops->symlink(root, "/tmp", "ext2_symlink_test"));

    struct vfs_dirent ent;
    struct vfs_node *node;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_symlink_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    char *buf = kmalloc(5, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(buf != NULL);

    FAIL_IF_FATAL(node->ops->readlink(node, buf, 4));

    TEST_ASSERT(strcmp(buf, "/tmp") == 0);

    flush();
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(ext2_dir_test, .group = TEST_GROUP(ext2),
                         TEST_INTENSITY(1, 1, 16)) {
    EXT2_INIT;

    FAIL_IF_FATAL(root->ops->mkdir(root, "ext2_dir_test", VFS_MODE_DIR));

    struct vfs_dirent ent;
    struct vfs_node *node;

    FAIL_IF_FATAL(root->ops->finddir(root, "ext2_dir_test", &ent));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    FAIL_IF_FATAL(root->ops->rmdir(root, "ext2_dir_test"));

    flush();
    return TEST_SUCCESS;
}
#endif
