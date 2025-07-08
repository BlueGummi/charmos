#include <block/generic.h>
#include <block/sched.h>
#include <crypto/prng.h>
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
    if (g_root_node->fs_type != FS_EXT2) {                                     \
        ADD_MESSAGE("the mounted root is not ext2");                           \
        SET_SKIP;                                                              \
        return;                                                                \
    }                                                                          \
    struct vfs_node *root = g_root_node;

static bool done2 = false;
static bool cb1d = false, cb2d = false;
static uint64_t avg_complete_time[BIO_SCHED_LEVELS] = {0};
static uint64_t total_complete_time[BIO_SCHED_LEVELS] = {0};
static uint64_t runs = 0;

static void bio_sch_callback(struct bio_request *req) {
    (void) req;

    done2 = true;
    uint64_t q_ms = (uint64_t) req->user_data >> 12;
    uint64_t q_lvl = (uint64_t) req->user_data & 7;
    uint64_t time = time_get_ms() - q_ms;
    total_complete_time[q_lvl] += time;
    req->user_data = NULL;
    runs++;
    TEST_ASSERT(req->status == BIO_STATUS_OK);
}

static void bio_sch_callback1(struct bio_request *req) {
    (void) req;

    cb1d = true;
    ADD_MESSAGE("cb 1 success");
}

static void bio_sch_callback2(struct bio_request *req) {
    (void) req;

    cb2d = true;
    ADD_MESSAGE("cb 2 success");
}

REGISTER_TEST(bio_sched_coalesce_test, IS_UNIT_TEST, SHOULD_NOT_FAIL) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct generic_disk *d = fs->drive;

    struct bio_request bio = {
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

    struct bio_request bio2 = {
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

    char *name = kmalloc(100);
    uint64_t t = time_get_us();
    bio_sched_enqueue(d, &bio);
    bio_sched_enqueue(d, &bio2);
    snprintf(name, 100, "enqueues took %d us", time_get_us() - t);
    ADD_MESSAGE(name);

    bio_sched_dispatch_all(d);
    sleep_ms(2);
    TEST_ASSERT(cb1d && cb2d);
    SET_SUCCESS;
}

REGISTER_TEST(bio_sched_delay_enqueue_test, IS_UNIT_TEST, SHOULD_NOT_FAIL) {
    EXT2_INIT;
    struct ext2_fs *fs = root->fs_data;
    struct generic_disk *d = fs->drive;

    prng_seed(time_get_us());

    uint64_t test_runs = 512;
    uint64_t runs_per_lvl[BIO_SCHED_LEVELS] = {0};
    uint8_t **buffers = kmalloc(test_runs * sizeof(uint8_t *));
    struct bio_request **rqs =
        kmalloc(test_runs * sizeof(struct bio_request *));

    for (uint64_t i = 0; i < test_runs; i++) {
        buffers[i] = kmalloc_aligned(512, PAGE_SIZE);
        rqs[i] = kzalloc(sizeof(struct bio_request));
        struct bio_request *rq = rqs[i];
        rq->disk = d;
        rq->lba = (i * 2) % 512;
        rq->sector_count = 1;
        rq->size = 512;
        rq->on_complete = bio_sch_callback;
        rq->buffer = buffers[i];
        rq->priority = prng_next() % BIO_SCHED_LEVELS;
        rq->write = false;
    }

    uint64_t ms = time_get_ms();
    for (uint64_t i = 0; i < test_runs; i++) {
        struct bio_request *rq = rqs[i];
        runs_per_lvl[rq->priority]++;
        rq->user_data = (void *) ((time_get_ms() << 12) | rq->priority);
        bio_sched_enqueue(d, rq);
    }
    ms = time_get_ms() - ms;

    for (uint64_t i = 0; i < BIO_SCHED_LEVELS; i++) {
        avg_complete_time[i] = total_complete_time[i] / runs_per_lvl[i];
        char *msg = kzalloc(100);
        snprintf(msg, 100, "Average completion time of level %d is %d ms", i,
                 avg_complete_time[i]);
        ADD_MESSAGE(msg);
    }

    char *msg = kmalloc(100);
    snprintf(msg, 100, "Total time spent is %d ms", ms);
    ADD_MESSAGE(msg);
    sleep_ms(2000);

    TEST_ASSERT(runs == test_runs);

    SET_SUCCESS;
}
