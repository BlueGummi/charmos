#include "test_internal.h"

#ifdef TEST_BIO_SCHED
TEST_GROUP_DECLARE(bio_sched);

#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        test_info("the mounted root is not ext2");                             \
        return TEST_SKIP(TEST_SKIP_NONE);                                      \
    }                                                                          \
    struct vfs_node *root = global.root_node;

static bool done2 = false;
static atomic_bool cb1d = false, cb2d = false;
static uint64_t avg_complete_time[BIO_SCHED_LEVELS] = {0};
static uint64_t total_complete_time[BIO_SCHED_LEVELS] = {0};
static _Atomic uint32_t runs = 0;

static void bio_sch_callback(struct bio_request *req) {
    (void) req;

    done2 = true;
    uint64_t q_ms = (uint64_t) req->user_data >> 12;
    uint64_t q_lvl = (uint64_t) req->user_data & 7;
    time_ms_t time = time_get_ms() - q_ms;
    total_complete_time[q_lvl] += time;
    req->user_data = NULL;
    atomic_fetch_add(&runs, 1);
    TEST_ASSERT_VOID(req->status == BIO_STATUS_OK);
}

static void bio_sch_callback1(struct bio_request *req) {
    (void) req;

    atomic_store(&cb1d, true);
    test_info("cb 1 success");
}

static void bio_sch_callback2(struct bio_request *req) {
    (void) req;

    atomic_store(&cb2d, true);
    test_info("cb 2 success");
}

TEST_DECLARE_UNIT(bio_sched_coalesce_test, .group = TEST_GROUP(bio_sched)) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct block_device *d = fs->drive;

    struct bio_request *bio = kmalloc(sizeof(*bio));
    *bio = (struct bio_request){
        .lba = 0,
        .disk = d,
        .buffer = kmalloc_aligned(512, 4096),
        .size = 512,
        .sector_count = 1,
        .write = false,
        .done = false,
        .status = -1,
        .on_complete = bio_sch_callback1,
        .priority = BIO_RQ_MEDIUM,
        .user_data = (void *) BIO_RQ_MEDIUM,
    };

    struct bio_request *bio2 = kmalloc(sizeof(*bio2));
    *bio2 = (struct bio_request){
        .lba = 1,
        .disk = d,
        .buffer = kmalloc_aligned(512, 4096),
        .size = 512,
        .sector_count = 1,
        .write = false,
        .done = false,
        .status = -1,
        .on_complete = bio_sch_callback2,
        .priority = BIO_RQ_MEDIUM,
        .user_data = (void *) BIO_RQ_MEDIUM,
    };

    bio->on_complete = bio_sch_callback1;
    bio2->on_complete = bio_sch_callback2;

    char *name = kmalloc(100);
    uint64_t t = time_get_us();
    bio_sched_enqueue(d, bio);
    bio_sched_enqueue(d, bio2);
    snprintf(name, 100, "enqueues took %d us", time_get_us() - t);
    test_info(name);

    bio_sched_dispatch_all(d);

    for (int i = 0; i < 5000; i++)
        scheduler_yield();
    return TEST_SUCCESS;
}
#endif
