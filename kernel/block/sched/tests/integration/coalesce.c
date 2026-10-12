#include "block/sched/tests/test_internal.h"

TEST_GROUP_DEFINE(bio_sched, .intensity_desc = {
                                 .curve = SCALE_PIECEWISE_LOG,
                                 .unit = "requests",
                             });

#define EXT2_ROOT struct vfs_node *root = global.root_node

static atomic_bool cb1d = false;

static void bio_sch_callback1(struct bio_request *req) {
    cc_unused(req);

    atomic_store(&cb1d, true);
    test_info("cb 1 success");
}

TEST_DEFINE_INTEGRATION(bio_sched, coalesce, TEST_INTENSITY(1, 2, 16),
                        .required_fs = FS_EXT2) {
    EXT2_ROOT;
    struct ext2_fs *fs = root->fs_data;
    struct block_device *d = fs->drive;
    size_t reqs = ctx->intensity_val ? ctx->intensity_val : 2;

    for (size_t i = 0; i < reqs; i++) {
        struct bio_request *req = vnew(req);
        *req = (struct bio_request){
            .lba = i,
            .disk = d,
            .buffer = valloc_aligned(512, 4096),
            .size = 512,
            .sector_count = 1,
            .write = false,
            .done = false,
            .status = -1,
            .on_complete = bio_sch_callback1,
            .priority = BIO_RQ_MEDIUM,
            .user_data = (void *) BIO_RQ_MEDIUM,
        };
        bio_sched_enqueue(d, req);
    }

    char *name = valloc(100);
    uint64_t t = time_get_us();
    snprintf(name, 100, "enqueues took %lu us", time_get_us() - t);
    test_info(name);

    bio_sched_dispatch_all(d);

    for (int i = 0; i < 5000; i++)
        scheduler_yield();

    return TEST_SUCCESS;
}
