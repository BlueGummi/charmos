#include "test_internal.h"

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

    TEST_ASSERT(bcache_total_dirty == 0);
}*/

static void flush() {
    struct ext2_fs *fs = global.root_node->fs_data;
    struct block_device *d = fs->drive;

    bio_sched_dispatch_all(d);

    /*
    sleep_ms(500);
    check_bcache();*/
}

TEST_DECLARE_INTEGRATION(ext2_integration_test, .group = TEST_GROUP(ext2)) {
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

    char *out_buf = kmalloc(len, ALLOC_FLAGS_ZERO);
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

    return TEST_SUCCESS;
}
#endif
