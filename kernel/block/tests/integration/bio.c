#include "../test_internal.h"

#ifdef TEST_BIO
TEST_GROUP_DECLARE(bio, .intensity_desc = {
                            .curve = SCALE_PIECEWISE_LINEAR,
                            .unit = "ios",
                        });

#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        test_info("the mounted root is not ext2");                             \
        return TEST_SKIP(TEST_SKIP_NONE);                                      \
    }                                                                          \
    struct vfs_node *root = global.root_node;

static atomic_bool done = false;
static void bio_callback(struct bio_request *req) {
    (void) req;
    done = true;
    test_info("blkdev_bio callback succeeded");
}

TEST_DECLARE_INTEGRATION(blkdev_bio_test, .group = TEST_GROUP(bio),
                         TEST_INTENSITY(1, 1, 16)) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct block_device *d = fs->drive;
    uint64_t run_times = ctx->intensity_val ? ctx->intensity_val : 1;
    done = false;
    enable_interrupts();

    for (uint64_t i = 0; i < run_times; i++) {
        struct bio_request *bio = kmalloc(sizeof(struct bio_request), 0);
        uint8_t *buf = kmalloc_aligned(64 * PAGE_SIZE, PAGE_SIZE);
        *bio = (struct bio_request){
            .lba = 0,
            .buffer = buf,
            .size = 512 * 512,
            .sector_count = 512,
            .write = false,
            .done = false,
            .status = -1,
            .on_complete = bio_callback,
            .priority = BIO_RQ_MEDIUM,
            .user_data = (void *) BIO_RQ_MEDIUM,
        };

        if (i % 2 == 0) {
            bio->priority = BIO_RQ_HIGH;
            bio->user_data = (void *) BIO_RQ_HIGH;
        }

        bool submitted = d->submit_bio_async(d, bio);
        if (!submitted)
            return TEST_FAIL("submit_bio_async rejected the request");

        sleep_spin_ms(100);

        TEST_ASSERT(bio->status == 0);
    }
    TEST_ASSERT(done == true);
    return TEST_SUCCESS;
}
#endif
