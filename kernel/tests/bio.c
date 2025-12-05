#ifdef TEST_BIO
#include <block/bio.h>
#include <block/generic.h>
#include <charmos.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <sleep.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>

#include "fs/detect.h"

#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        ADD_MESSAGE("the mounted root is not ext2");                           \
        SET_SKIP();                                                            \
        return;                                                                \
    }                                                                          \
    struct vfs_node *root = global.root_node;

static bool done = false;

static void bio_callback(struct bio_request *req) {
    (void) req;
    done = true;
    ADD_MESSAGE("blkdev_bio callback succeeded");
}

REGISTER_TEST(blkdev_bio_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct generic_disk *d = fs->drive;
    uint64_t run_times = 1;
    enable_interrupts();

    for (uint64_t i = 0; i < run_times; i++) {
        struct bio_request *bio =
            kmalloc(sizeof(struct bio_request), ALLOC_PARAMS_DEFAULT);
        *bio = (struct bio_request) {
            .lba = 0,
            .buffer = kmalloc_aligned(64 * PAGE_SIZE, PAGE_SIZE,
                                      ALLOC_PARAMS_DEFAULT),
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
            SET_SKIP();
            ADD_MESSAGE("BIO function is NULL");
            return;
        }

        bool submitted = d->submit_bio_async(d, bio);
        if (!submitted) {
            SET_FAIL();
            return;
        }

        sleep_ms(100);

        TEST_ASSERT(bio->status == 0);
    }
    TEST_ASSERT(done == true);
    TEST_ASSERT(current_test->message_count == run_times);
    SET_SUCCESS();
}
#endif
