#include "../test_internal.h"

#ifdef TEST_BIO_SCHED
#define EXT2_INIT                                                              \
    if (global.root_node->fs_type != FS_EXT2) {                                \
        test_info("the mounted root is not ext2");                             \
        return TEST_SKIP(TEST_SKIP_NONE);                                      \
    }                                                                          \
    struct vfs_node *root = global.root_node;

static bool done2 = false;
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

#define BIO_SCHED_TEST_RUNS_MAX 4096
static uint64_t runs_per_lvl[BIO_SCHED_LEVELS] = {0};
static struct bio_request *rqs[BIO_SCHED_TEST_RUNS_MAX] = {0};
static uint8_t *buffers[BIO_SCHED_TEST_RUNS_MAX] = {0};

TEST_DECLARE_INTEGRATION(bio_sched_delay_enqueue_test,
                         .group = TEST_GROUP(bio_sched),
                         TEST_INTENSITY(64, 1024, 4096)) {
    EXT2_INIT;
    ABORT_IF_RAM_LOW();

    struct ext2_fs *fs = root->fs_data;
    struct block_device *d = fs->drive;
    kassert(d);

    size_t test_runs = ctx->intensity_val ? ctx->intensity_val : 1024;
    if (test_runs > BIO_SCHED_TEST_RUNS_MAX)
        test_runs = BIO_SCHED_TEST_RUNS_MAX;

    memset(runs_per_lvl, 0, sizeof(runs_per_lvl));
    memset(total_complete_time, 0, sizeof(total_complete_time));
    memset(avg_complete_time, 0, sizeof(avg_complete_time));
    atomic_store(&runs, 0);

    prng_seed(ctx->seed ? ctx->seed : time_get_us());

    for (uint64_t i = 0; i < test_runs; i++) {
        uint8_t *buf = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        struct bio_request *rq =
            kmalloc(sizeof(struct bio_request), ALLOC_FLAGS_ZERO);
        TEST_ASSERT(rq && buf);
        TEST_ASSERT(IS_ALIGNED((vaddr_t) buf, PAGE_SIZE));

        rq->disk = d;
        rq->lba = (i * 2) % 512;
        rq->sector_count = 1;
        rq->size = 512;
        rq->on_complete = bio_sch_callback;
        rq->buffer = buf;
        rq->priority = prng_next() % BIO_SCHED_LEVELS;
        rq->write = false;
        INIT_LIST_HEAD(&rq->list);

        rqs[i] = rq;
        buffers[i] = buf;
    }

    for (size_t i = 0; i < test_runs; i++)
        for (size_t j = 0; j < test_runs; j++)
            if (i != j && rqs[i] == rqs[j])
                test_err("duplicate at %u and %u\n", i, j);

    for (size_t i = 0; i < test_runs; i++) {
        if (!rqs[i]->disk) {
            test_err("rq %p %u\n", rqs[i], i);
            return TEST_SUCCESS;
        }

        kassert(rqs[i]->disk);
    }

    uint64_t ms = time_get_ms();
    for (uint64_t i = 0; i < test_runs; i++) {
        struct bio_request *rq = rqs[i];
        runs_per_lvl[rq->priority]++;
        rq->user_data = (void *) ((time_get_ms() << 12) | rq->priority);
        bio_sched_enqueue(d, rq);
    }
    ms = time_get_ms() - ms;

    char *msg = kmalloc(100);
    TEST_ASSERT(msg);
    snprintf(msg, 100, "Total time spent enqueuing is %d ms", ms);
    test_info(msg);

    bio_sched_dispatch_all(d);

    for (uint64_t i = 0; i < 150000; i++)
        cpu_relax();

    for (uint64_t i = 0; i < BIO_SCHED_LEVELS; i++) {
        if (runs_per_lvl[i] > 0)
            avg_complete_time[i] = total_complete_time[i] / runs_per_lvl[i];
        else
            avg_complete_time[i] = 0;
        char *lvl_msg = kmalloc(100, ALLOC_FLAGS_ZERO);
        TEST_ASSERT(lvl_msg);
        snprintf(lvl_msg, 100, "Average completion time of level %d is %d ms",
                 i, avg_complete_time[i]);
        test_info(lvl_msg);
    }

    char *m2 = kmalloc(100);
    TEST_ASSERT(m2);
    snprintf(m2, 100, "Runs is %d, test_runs is %zu", atomic_load(&runs),
             test_runs);
    test_info(m2);
    TEST_ASSERT(atomic_load(&runs) <= test_runs);

    return TEST_SUCCESS;
}
#endif
