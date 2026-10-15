#include "block/tests/test_internal.h"

#include <mem/nofail.h>
#include <thread/thread.h>
#include <time/time.h>

/* The completion has to reach the driver's worker thread and back, so the
 * budget is loose */
#define BIO_COMPLETE_TIMEOUT_MS 2000
#define BIO_POLL_INTERVAL_US 200

TEST_GROUP_DEFINE(bio, .intensity_desc = {
                           .curve = SCALE_PIECEWISE_LINEAR,
                           .unit = "ios",
                       });

#define EXT2_ROOT struct vfs_node *root = global.root_node

static atomic_bool done = false;
static void bio_callback(struct bio_request *req) {
    cc_ignore(req);
    atomic_store(&done, true);
}

TEST_DEFINE_INTEGRATION(bio, async_submit, TEST_INTENSITY(1, 1, 16),
                        .required_fs = FS_EXT2) {
    EXT2_ROOT;
    struct ext2_fs *fs = root->fs_data;
    struct block_device *d = fs->drive;
    uint64_t run_times = ctx->intensity_val ? ctx->intensity_val : 1;
    time_ms_t worst_ms = 0;
    atomic_store(&done, false);
    irq_enable_untracked();

    for (uint64_t i = 0; i < run_times; i++) {
        /* nofail, not TEST_ASSERT: earlier requests may still be in flight */
        struct bio_request *bio = nofail_vnew(bio, .flags = 0);
        uint8_t *buf = nofail(valloc_aligned(64 * PAGE_SIZE, PAGE_SIZE));
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

        /* Reset per request */
        atomic_store(&done, false);

        bool submitted = d->submit_bio_async(d, bio);
        if (!submitted)
            return TEST_FAIL("submit_bio_async rejected the request");

        time_ms_t start = time_get_ms();
        while (!atomic_load(&done)) {
            if (time_get_ms() - start >= BIO_COMPLETE_TIMEOUT_MS) {
                test_err("request %llu never completed: %u ms elapsed, "
                         "status still %d",
                         (unsigned long long) i, BIO_COMPLETE_TIMEOUT_MS,
                         bio->status);
                return TEST_FAIL("bio did not complete before the deadline");
            }
            thread_sleep_for_us(BIO_POLL_INTERVAL_US);
        }

        time_ms_t took = time_get_ms() - start;
        if (took > worst_ms)
            worst_ms = took;

        TEST_ASSERT_OK(bio->status);

        /* Now that the request is known to be finished, its buffers can go
         * back */
        vfree_aligned(buf);
        vfree(bio);
    }

    test_info("%llu request(s), worst completion %llu ms",
              (unsigned long long) run_times, (unsigned long long) worst_ms);

    TEST_ASSERT_TRUE(atomic_load(&done));
    return TEST_SUCCESS;
}
