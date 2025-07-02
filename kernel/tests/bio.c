#include <devices/generic_disk.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <sleep.h>
#include <tests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/detect.h"

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
    done = true;
    ADD_MESSAGE("blkdev_bio callback succeeded");
}

REGISTER_TEST(blkdev_bio_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct generic_disk *d = fs->drive;
    uint64_t run_times = 1;

    for (uint64_t i = 0; i < run_times; i++) {
        struct bio_request bio = {
            .lba = 0,
            .buffer = kmalloc_aligned(512, 4096),
            .size = 512,
            .sector_count = 512 * 512,
            .write = false,
            .done = false,
            .status = -1,
            .on_complete = bio_callback,
            .user_data = NULL,
        };

        if (!d->submit_bio_async) {
            SET_SKIP;
            ADD_MESSAGE("BIO function is NULL");
            return;
        }

        bool submitted = d->submit_bio_async(d, &bio);
        if (!submitted) {
            kfree_aligned(bio.buffer);
            SET_FAIL;
            return;
        }

        sleep_ms(3);

        TEST_ASSERT(bio.status == 0);
        kfree_aligned(bio.buffer);
    }
    TEST_ASSERT(done == true);
    TEST_ASSERT(current_test->message_count == run_times);
    SET_SUCCESS;
}
