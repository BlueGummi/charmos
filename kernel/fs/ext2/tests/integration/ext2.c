#include "fs/ext2/tests/test_internal.h"

#ifdef TEST_EXT2

#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        test_info("the mounted root is not ext2");                             \
        return TEST_SKIP(TEST_SKIP_NONE);                                      \
    }                                                                          \
    struct vfs_node *root = global.root_node;

/*
static void check_bcache(void) {
    struct ext2_fs *fs = g_root_node->fs_data;
    struct generic_disk *d = fs->drive;

    uint64_t bcache_total_dirty = 42;
    uint64_t bcache_total_present = 37;

    bcache_stat(d, &bcache_total_dirty, &bcache_total_present);

    char *msg = kmalloc(100);
    snprintf(msg, 100, "Block cache has %d dirty entries and %d total entries",
             bcache_total_dirty, bcache_total_present);

    test_info(msg);

    TEST_ASSERT_EQ(bcache_total_dirty, 0);
}*/

static void flush() {
    struct ext2_fs *fs = global.root_node->fs_data;
    struct block_device *d = fs->drive;

    bio_sched_dispatch_all(d);

    /*
    sleep_ms(500);
    check_bcache();*/
}

TEST_DECLARE_INTEGRATION(ext2, file_lifecycle, TEST_INTENSITY(1, 4, 64)) {
    EXT2_INIT;

    size_t ops = ctx->intensity_val ? ctx->intensity_val : 4;
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);
    char *out_buf = kmalloc(len + 1, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(out_buf);

    for (size_t iter = 0; iter < ops; iter++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "ext2_test_%zu", iter);

        TEST_ASSERT(
            !ERR_IS_FATAL(root->ops->create(root, fname, VFS_MODE_FILE)));

        struct vfs_dirent ent;
        struct vfs_node *node;
        TEST_ASSERT(!ERR_IS_FATAL(root->ops->finddir(root, fname, &ent)));

        node = ent.node;
        TEST_ASSERT_NONNULL(node);

        TEST_ASSERT(!ERR_IS_FATAL(node->ops->write(node, lstr, len, 0)));
        TEST_ASSERT_EQ(node->size, len);

        memset(out_buf, 0, len + 1);
        TEST_ASSERT(!ERR_IS_FATAL(node->ops->read(node, out_buf, len, 0)));
        TEST_ASSERT_MEM_EQ(out_buf, lstr, len);

        TEST_ASSERT(!ERR_IS_FATAL(node->ops->truncate(node, len / 2)));

        memset(out_buf, 0, len + 1);
        TEST_ASSERT(!ERR_IS_FATAL(node->ops->read(node, out_buf, len, 0)));
        TEST_ASSERT_EQ(strlen(out_buf), len / 2);

        TEST_ASSERT(!ERR_IS_FATAL(node->ops->unlink(root, fname)));

        enum errno e = root->ops->finddir(root, fname, &ent);
        TEST_ASSERT_EQ_S(e, ERR_NO_ENT);
    }

    kfree(out_buf);
    flush();

    return TEST_SUCCESS;
}
#endif
