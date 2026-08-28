#include "../test_internal.h"

#ifdef TEST_TMPFS
TEST_GROUP_DECLARE(tmpfs, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "ops",
                          });

#define TMPFS_SETUP_NODE(root, node, name, e)                                  \
    struct vfs_node *root = tmpfs_mkroot("tmp");                               \
    TEST_ASSERT_NONNULL(root);                                                 \
    TEST_ASSERT(!ERR_IS_FATAL(root->ops->create(root, name, VFS_MODE_FILE)));  \
    struct vfs_dirent ent;                                                     \
    struct vfs_node *node;                                                     \
    TEST_ASSERT(!ERR_IS_FATAL(root->ops->finddir(root, name, &ent)));          \
    node = ent.node;                                                           \
    TEST_ASSERT_NONNULL(node);

TEST_DECLARE_INTEGRATION(tmpfs, tmpfs_rw_test, TEST_INTENSITY(1, 16, 256)) {
    size_t ops = ctx->intensity_val ? ctx->intensity_val : 16;
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    char *out_buf = kmalloc(len + 1, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(out_buf);

    for (size_t iter = 0; iter < ops; iter++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "place_%zu", iter);

        struct vfs_node *root = tmpfs_mkroot("tmp");
        TEST_ASSERT_NONNULL(root);
        TEST_ASSERT(
            !ERR_IS_FATAL(root->ops->create(root, fname, VFS_MODE_FILE)));
        struct vfs_dirent ent;
        struct vfs_node *node;
        TEST_ASSERT(!ERR_IS_FATAL(root->ops->finddir(root, fname, &ent)));
        node = ent.node;
        TEST_ASSERT_NONNULL(node);
        TEST_ASSERT_EQ(node->size, 0);

        TEST_ASSERT(!ERR_IS_FATAL(node->ops->write(node, lstr, len, 0)));
        TEST_ASSERT_EQ(node->size, len);

        memset(out_buf, 0, len + 1);
        TEST_ASSERT(!ERR_IS_FATAL(node->ops->read(node, out_buf, len, 0)));
        TEST_ASSERT_MEM_EQ(out_buf, lstr, len);

        TEST_ASSERT(!ERR_IS_FATAL(node->ops->truncate(node, len / 2)));
        TEST_ASSERT_EQ(node->size, len / 2);

        memset(out_buf, 0, len + 1);
        TEST_ASSERT(!ERR_IS_FATAL(node->ops->read(node, out_buf, len, 0)));
        TEST_ASSERT(!ERR_IS_FATAL(node->ops->unlink(root, fname)));

        enum errno e = root->ops->finddir(root, fname, &ent);
        TEST_ASSERT_EQ(e, ERR_NO_ENT);

        TEST_ASSERT_EQ(strlen(out_buf), len / 2);
    }

    kfree(out_buf);
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(tmpfs, tmpfs_dir_test, TEST_INTENSITY(1, 8, 128)) {
    size_t ops = ctx->intensity_val ? ctx->intensity_val : 8;
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    char *out_buf = kmalloc(len + 1, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(out_buf);

    for (size_t iter = 0; iter < ops; iter++) {
        char dname[32];
        snprintf(dname, sizeof(dname), "place_%zu", iter);

        struct vfs_node *root = tmpfs_mkroot("tmp");
        TEST_ASSERT_NONNULL(root);

        TEST_ASSERT(!ERR_IS_FATAL(root->ops->mkdir(root, dname, VFS_MODE_DIR)));

        struct vfs_dirent ent;
        struct vfs_node *dir;

        TEST_ASSERT(!ERR_IS_FATAL(root->ops->finddir(root, dname, &ent)));

        dir = ent.node;
        TEST_ASSERT_NONNULL(dir);

        enum errno e = dir->ops->write(dir, lstr, len, 0);
        TEST_ASSERT_EQ(e, ERR_IS_DIR);

        e = dir->ops->read(dir, out_buf, len, 0);
        TEST_ASSERT_EQ(e, ERR_IS_DIR);

        TEST_ASSERT(!ERR_IS_FATAL(dir->ops->rmdir(root, dname)));

        e = root->ops->finddir(root, dname, &ent);
        TEST_ASSERT_EQ(e, ERR_NO_ENT);
    }

    kfree(out_buf);
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(tmpfs, tmpfs_general_tests) {
    TMPFS_SETUP_NODE(root, node, "place", e);

    TEST_ASSERT(!ERR_IS_FATAL(node->ops->chmod(node, VFS_MODE_EXEC)));

    TEST_ASSERT_EQ(node->mode, VFS_MODE_EXEC);

    TEST_ASSERT(!ERR_IS_FATAL(node->ops->chown(node, 42, 37)));

    TEST_ASSERT_EQ(node->uid, 42);
    TEST_ASSERT_EQ(node->gid, 37);

    TEST_ASSERT(
        !ERR_IS_FATAL(root->ops->mkdir(root, "bingbong", VFS_MODE_DIR)));
    TEST_ASSERT(!ERR_IS_FATAL(root->ops->finddir(root, "bingbong", &ent)));

    node = ent.node;
    TEST_ASSERT_NONNULL(node);

    TEST_ASSERT(!ERR_IS_FATAL(node->ops->symlink(node, "/tmp", "bang")));

    struct vfs_node *bang;
    TEST_ASSERT(!ERR_IS_FATAL(node->ops->finddir(node, "bang", &ent)));

    bang = ent.node;
    TEST_ASSERT_NONNULL(bang);

    char *buf = kmalloc(10, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(buf);

    TEST_ASSERT(!ERR_IS_FATAL(bang->ops->readlink(bang, buf, 10)));

    TEST_ASSERT_STR_EQ(buf, "/tmp");

    kfree(buf);
    return TEST_SUCCESS;
}
#endif
