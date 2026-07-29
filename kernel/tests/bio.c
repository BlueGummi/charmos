#ifdef TEST_BIO
#include <block/bio.h>
#include <block/block.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <global.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <test.h>
#include <time/spin_sleep.h>

#include "fs/detect.h"

TEST_GROUP_DECLARE(bio);

#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        test_info("the mounted root is not ext2");                             \
        return TEST_SKIP(TEST_SKIP_NONE);                                      \
    }                                                                          \
    struct vfs_node *root = global.root_node;

static bool done = false;

static void bio_callback(struct bio_request *req) {
    (void) req;
    done = true;
    test_info("blkdev_bio callback succeeded");
}

TEST_DECLARE_UNIT(blkdev_bio_test, .group = TEST_GROUP(bio)) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct block_device *d = fs->drive;
    uint64_t run_times = 1;
    enable_interrupts();

    for (uint64_t i = 0; i < run_times; i++) {
        struct bio_request *bio = kmalloc(sizeof(struct bio_request));
        *bio = (struct bio_request){
            .lba = 0,
            .buffer = kmalloc_aligned(64 * PAGE_SIZE, PAGE_SIZE),
            .size = 512 * 512,
            .sector_count = 512,
            .write = false,
            .done = false,
            .status = -1,
            .on_complete = bio_callback,
            .user_data = NULL,
            .disk = d,
        };
        INIT_LIST_HEAD(&bio->list);

        if (!d->submit_bio_async) {
            test_info("BIO function is NULL");
            return TEST_SKIP(TEST_SKIP_NONE);
        }

        bool submitted = d->submit_bio_async(d, bio);
        if (!submitted) {
            return TEST_FAIL("submit_bio_async rejected the request");
        }

        sleep_spin_ms(100);

        TEST_ASSERT(bio->status == 0);
    }
    TEST_ASSERT(done == true);
    TEST_ASSERT(test_current_message_count() == run_times);
    return TEST_SUCCESS;
}
#endif
