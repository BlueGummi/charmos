#include <block/generic.h>
#include <block/sched.h>
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
static uint64_t sch_start_ms = 0, sch_end_ms = 0;

static void bio_sch_callback(struct bio_request *req) {
    (void) req;

    done2 = true;
    sch_end_ms = time_get_ms();
    char *msg = kmalloc(100);
    snprintf(msg, 100, "bio_sch_callback succeeded in %d ms",
             sch_end_ms - sch_start_ms);
    ADD_MESSAGE(msg);
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
    sch_start_ms = time_get_ms();
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
        .on_complete = bio_sch_callback,
        .priority = BIO_RQ_HIGH,
        .user_data = (void *) BIO_RQ_HIGH,
    };

    struct bio_request bio2 = {
        .lba = 50,
        .disk = d,
        .buffer = kmalloc_aligned(512, 4096),
        .size = 512,
        .sector_count = 1,
        .write = false,
        .done = false,
        .status = -1,
        .on_complete = bio_sch_callback,
        .priority = BIO_RQ_BACKGROUND,
        .user_data = (void *) BIO_RQ_BACKGROUND,
    };

    struct bio_request bio3 = {
        .lba = 50,
        .disk = d,
        .buffer = kmalloc_aligned(512, 4096),
        .size = 512,
        .sector_count = 1,
        .write = false,
        .done = false,
        .status = -1,
        .on_complete = bio_sch_callback,
        .priority = BIO_RQ_LOW,
        .user_data = (void *) BIO_RQ_LOW,
    };

    bio_sched_enqueue(d, &bio);
    bio_sched_enqueue(d, &bio2);
    bio_sched_enqueue(d, &bio3);

    sleep_ms(400);

    TEST_ASSERT(current_test->message_count == 3);
    SET_SUCCESS;
}
