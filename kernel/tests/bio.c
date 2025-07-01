#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <sleep.h>
#include <tests.h>

#define EXT2_INIT                                                              \
    if (g_root_node->fs_type != FS_EXT2) {                                     \
        ADD_MESSAGE("the mounted root is not ext2");                           \
        SET_SKIP;                                                              \
        return;                                                                \
    }                                                                          \
    struct vfs_node *root = g_root_node;

static bool done = false;

void bio_callback(struct bio_request *req) {
    (void) req;
    ADD_MESSAGE("blkdev_bio callback succeeded");
    done = true;
}

REGISTER_TEST(blkdev_bio_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct generic_disk *d = fs->drive;

    struct bio_request bio = {
        .lba = 0,
        .buffer = kmalloc_aligned(512, 4096),
        .size = 512,
        .sector_count = 1,
        .write = false,
        .done = false,
        .status = -1,
        .on_complete = bio_callback,
        .user_data = NULL,
        .wait_queue = {0},
    };

    bool submitted = d->submit_bio_async(d, &bio);
    if (!submitted) {
        kfree_aligned(bio.buffer);
        SET_FAIL;
        return;
    }

    uint64_t timeout = 100 * 1000;
    while (!done && timeout--) {
        sleep_us(10);
        if (timeout == 0) {
            SET_FAIL;
        }
    }

    TEST_ASSERT(bio.status == 0);
    TEST_ASSERT(done == true);

    kfree_aligned(bio.buffer);
    SET_SUCCESS;
}
