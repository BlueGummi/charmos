#include "../test_internal.h"

#ifdef TEST_STACK_DEPOT
static __noinline void sd_save_n(stack_handle_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        out[i] = stack_depot_save_current();
}


#define SD_SEED 0xDEADBEEFULL
#define SD_TRACE_LEN 8
#define SD_MANY 4096 /* > STACK_DEPOT_HASH_SIZE, forces chain collisions */
static_assert(SD_MANY > STACK_DEPOT_HASH_SIZE);

static void sd_make_trace(uintptr_t *entries, size_t len, uint64_t id) {
    for (size_t i = 0; i < len; i++)
        entries[i] = (uintptr_t) (0xffffffff80000000ULL + (id << 20) + i * 16);
}


#define SD_MT_THREADS 8
#define SD_MT_TIMEOUT_MS 30000

struct sd_mt_state {
    atomic_uint left;
    atomic_bool stop;
    atomic_bool fail;
    atomic_bool oom;
    const char *fail_msg;
    uint64_t seed;

    struct thread *threads[SD_MT_THREADS];
    size_t nthreads;
};

static struct sd_mt_state sd_mt;

static void sd_mt_reset(struct test_context *ctx, unsigned workers) {
    atomic_store(&sd_mt.left, workers);
    atomic_store(&sd_mt.stop, false);
    atomic_store(&sd_mt.fail, false);
    atomic_store(&sd_mt.oom, false);
    sd_mt.fail_msg = NULL;
    sd_mt.seed = ctx->seed;
    memset(sd_mt.threads, 0, sizeof(sd_mt.threads));
    sd_mt.nthreads = 0;
}

static void sd_mt_report(const char *msg) {
    /* First failure wins; everyone else just stops. */
    if (!atomic_exchange(&sd_mt.fail, true))
        sd_mt.fail_msg = msg;
    atomic_store(&sd_mt.stop, true);
}

static void sd_mt_report_oom(void) {
    atomic_store(&sd_mt.oom, true);
    atomic_store(&sd_mt.stop, true);
}

#define SD_WORKER_CHECK(x)                                                     \
    do {                                                                       \
        if (!(x)) {                                                            \
            sd_mt_report(#x " (worker, " __RELFILE__ ")");                     \
            return false;                                                      \
        }                                                                      \
    } while (0)

static inline uint64_t sd_rng(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*s = x);
}

static inline uint64_t sd_rng_seed(size_t tid) {
    return (sd_mt.seed ^ (0x9E3779B97F4A7C15ULL * (tid + 1))) | 1;
}

static size_t sd_chain_count(uintptr_t *trace, size_t len) {
    struct stack_depot_record_chain *chain =
        &stack_depot_global
             .chains[stack_depot_hash(trace, len) % STACK_DEPOT_HASH_SIZE];
    struct stack_depot_record *pos;
    size_t n = 0;

    enum irql irql = spin_lock(&chain->lock);
    list_for_each_entry(pos, &chain->list,
                        hash_list) if (pos->num_entries == len &&
                                       !memcmp(pos->entries, trace,
                                               len * sizeof(uintptr_t))) n++;
    spin_unlock(&chain->lock, irql);

    return n;
}

static bool sd_mt_wait_for(atomic_uint *counter, unsigned target) {
    time_ms_t deadline = time_get_ms() + SD_MT_TIMEOUT_MS;

    while (atomic_load(counter) != target) {
        if (time_get_ms() > deadline)
            return false;
        scheduler_yield();
    }

    return true;
}

static void sd_mt_abandon(void) {
    atomic_store(&sd_mt.stop, true);

    for (size_t i = 0; i < sd_mt.nthreads; i++) {
        if (sd_mt.threads[i]) {
            thread_detach(sd_mt.threads[i]);
            sd_mt.threads[i] = NULL;
        }
    }
}

/* Join + fold verdict */
static struct test_verdict sd_mt_join(void) {
    time_ms_t deadline = time_get_ms() + SD_MT_TIMEOUT_MS;
    bool timed_out = false;

    for (size_t i = 0; i < sd_mt.nthreads; i++) {
        struct thread *t = sd_mt.threads[i];
        if (!t)
            continue;

        sd_mt.threads[i] = NULL;

        time_ms_t now = time_get_ms();
        time_ms_t left = now >= deadline ? 1 : deadline - now;

        /* If it times out, we stop waiting on the rest too */
        if (timed_out || !thread_join_timeout(t, left, NULL)) {
            atomic_store(&sd_mt.stop, true);
            thread_detach(t);
            timed_out = true;
        }
    }

    if (timed_out)
        return TEST_FAIL("workers did not finish in time");

    if (atomic_load(&sd_mt.fail))
        return TEST_FAIL(sd_mt.fail_msg);

    if (atomic_load(&sd_mt.oom))
        return TEST_SKIP(TEST_SKIP_RAM_LOW);

    /* every worker ran to completion */
    if (atomic_load(&sd_mt.left))
        return TEST_FAIL("worker fleet did not spawn");

    return TEST_SUCCESS;
}

#define SD_MT_JOIN()                                                           \
    do {                                                                       \
        struct test_verdict _v = sd_mt_join();                                 \
        if (_v.result != TEST_RESULT_OK)                                       \
            return _v;                                                         \
    } while (0)

static void sd_mt_spawn(char *name, void (*fn)(void *), size_t n) {
    kassert(n <= SD_MT_THREADS);
    sd_mt.nthreads = n;

    /* Raise to spawn a bunch */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < n; i++)
        sd_mt.threads[i] =
            thread_spawn_joinable(name, fn, (void *) (uintptr_t) i);
    irql_lower(irql);
}

#define SD_MT_DEDUP_SAVES_MAX 256
#define SD_MT_DEDUP_ID 0x3000

static stack_handle_t sd_dedup_handles[SD_MT_THREADS];
static atomic_uint sd_dedup_saved;
static atomic_bool sd_dedup_release;
static size_t sd_dedup_saves_count = 64;

static bool sd_dedup_body(size_t tid, stack_handle_t *held, size_t *held_n) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, SD_MT_DEDUP_ID);

    for (size_t i = 0; i < sd_dedup_saves_count; i++) {
        stack_handle_t h =
            stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!h) {
            sd_mt_report_oom();
            return false;
        }

        held[(*held_n)++] = h;
        /* Every save of an identical trace must land on one record. */
        SD_WORKER_CHECK(h == held[0]);
    }

    sd_dedup_handles[tid] = held[0];
    return true;
}

static void sd_dedup_worker(void *arg) {
    size_t tid = (size_t) (uintptr_t) arg;
    stack_handle_t held[SD_MT_DEDUP_SAVES_MAX];
    size_t held_n = 0;

    sd_dedup_body(tid, held, &held_n);

    /* Publish unconditionally since the main thread will be waiting */
    atomic_fetch_add(&sd_dedup_saved, 1);

    while (!atomic_load(&sd_dedup_release))
        scheduler_yield();

    for (size_t i = 0; i < held_n; i++)
        stack_depot_put(held[i]);

    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_dedup,
                         .group = TEST_GROUP(stack_depot),
                         TEST_INTENSITY(16, 64, 256)) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, SD_MT_DEDUP_ID);
    TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);

    sd_dedup_saves_count = ctx->intensity_val ? ctx->intensity_val : 64;
    if (sd_dedup_saves_count > SD_MT_DEDUP_SAVES_MAX)
        sd_dedup_saves_count = SD_MT_DEDUP_SAVES_MAX;

    sd_mt_reset(ctx, SD_MT_THREADS);
    atomic_store(&sd_dedup_saved, 0);
    atomic_store(&sd_dedup_release, false);
    memset(sd_dedup_handles, 0, sizeof(sd_dedup_handles));

    sd_mt_spawn("sd_dedup", sd_dedup_worker, SD_MT_THREADS);

    /* All refs are taken and none are released yet, since the depot
     * must hold precisely one record with every ref accounted */
    bool barrier = sd_mt_wait_for(&sd_dedup_saved, SD_MT_THREADS);
    bool clean = !atomic_load(&sd_mt.fail) && !atomic_load(&sd_mt.oom);

    if (barrier && clean) {
        stack_handle_t h = sd_dedup_handles[0];
        if (h) {
            struct stack_depot_record *rec = stack_depot_get_record(h);
            if (refcount_read(&rec->refcount) !=
                SD_MT_THREADS * sd_dedup_saves_count)
                sd_mt_report("refcount != total concurrent saves");

            for (size_t i = 1; i < SD_MT_THREADS; i++)
                if (sd_dedup_handles[i] != h)
                    sd_mt_report("threads got distinct records for one trace");

            if (sd_chain_count(trace, SD_TRACE_LEN) != 1)
                sd_mt_report("duplicate records on chain");
        }
    }

    atomic_store(&sd_dedup_release, true);

    if (!barrier) {
        sd_mt_abandon();
        return TEST_FAIL("workers did not reach the barrier in time");
    }

    SD_MT_JOIN();

    /* Last put drops the record off the chain */
    TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
    return TEST_SUCCESS;
}

#define SD_MT_SET 16
#define SD_MT_SET_ID 0x4000
static size_t sd_mt_set_iters_count = 1500;

static bool sd_shared_body(size_t tid) {
    uint64_t rng = sd_rng_seed(tid);

    for (size_t i = 0; i < sd_mt_set_iters_count; i++) {
        if (atomic_load(&sd_mt.stop))
            return false;

        size_t id = sd_rng(&rng) % SD_MT_SET;
        uintptr_t want[SD_TRACE_LEN];
        sd_make_trace(want, SD_TRACE_LEN, SD_MT_SET_ID + id);

        stack_handle_t h =
            stack_depot_save(want, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!h) {
            sd_mt_report_oom();
            return false;
        }

        struct stack_depot_record *rec = stack_depot_get_record(h);
        /* While we hold a ref the record has to be ours, so 
         * we check the data to make sure */
        SD_WORKER_CHECK(rec->num_entries == SD_TRACE_LEN);
        SD_WORKER_CHECK(rec->hash == stack_depot_hash(want, SD_TRACE_LEN));
        SD_WORKER_CHECK(refcount_read(&rec->refcount) > 0);

        uintptr_t got[STACK_TRACE_MAX_DEPTH];
        size_t n = stack_depot_read(h, got);
        SD_WORKER_CHECK(n == SD_TRACE_LEN);
        SD_WORKER_CHECK(!memcmp(got, want, sizeof(want)));

        if (sd_rng(&rng) & 1)
            scheduler_yield();

        /* Re read after potential preemption, so racing put does not
         * recycle the record under our ref */
        SD_WORKER_CHECK(stack_depot_read(h, got) == SD_TRACE_LEN);
        SD_WORKER_CHECK(!memcmp(got, want, sizeof(want)));

        stack_depot_put(h);
    }

    return true;
}

static void sd_shared_worker(void *arg) {
    sd_shared_body((size_t) (uintptr_t) arg);
    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_shared_set,
                         .group = TEST_GROUP(stack_depot),
                         TEST_INTENSITY(200, 1500, 8000)) {
    sd_mt_set_iters_count = ctx->intensity_val ? ctx->intensity_val : 1500;
    sd_mt_reset(ctx, SD_MT_THREADS);
    sd_mt_spawn("sd_shared", sd_shared_worker, SD_MT_THREADS);
    SD_MT_JOIN();

    /* Everything was put back and nothing left behind */
    for (size_t id = 0; id < SD_MT_SET; id++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, SD_MT_SET_ID + id);
        TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
    }

    return TEST_SUCCESS;
}

#define SD_MT_DISJOINT_PER_THREAD_MAX 128
#define SD_MT_DISJOINT_ID 0x5000
static size_t sd_mt_disjoint_per_thread = 32;

static stack_handle_t sd_disjoint_handles[SD_MT_THREADS]
                                         [SD_MT_DISJOINT_PER_THREAD_MAX];

static bool sd_disjoint_body(size_t tid) {
    stack_handle_t *mine = sd_disjoint_handles[tid];

    for (size_t i = 0; i < sd_mt_disjoint_per_thread; i++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN,
                      SD_MT_DISJOINT_ID + tid * sd_mt_disjoint_per_thread + i);

        mine[i] = stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!mine[i]) {
            sd_mt_report_oom();
            return false;
        }

        /* No one else uses this id, and we are the only reference */
        SD_WORKER_CHECK(
            refcount_read(&stack_depot_get_record(mine[i])->refcount) == 1);

        stack_handle_t again =
            stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        SD_WORKER_CHECK(again == mine[i]);
        SD_WORKER_CHECK(
            refcount_read(&stack_depot_get_record(mine[i])->refcount) == 2);
        stack_depot_put(again);

        if ((i & 3) == 0)
            scheduler_yield();
    }

    /* Re-verify after every thread has been hammering the same chains. */
    for (size_t i = 0; i < sd_mt_disjoint_per_thread; i++) {
        uintptr_t want[SD_TRACE_LEN], got[STACK_TRACE_MAX_DEPTH];
        sd_make_trace(want, SD_TRACE_LEN,
                      SD_MT_DISJOINT_ID + tid * sd_mt_disjoint_per_thread + i);

        SD_WORKER_CHECK(stack_depot_read(mine[i], got) == SD_TRACE_LEN);
        SD_WORKER_CHECK(!memcmp(got, want, sizeof(want)));
        SD_WORKER_CHECK(
            refcount_read(&stack_depot_get_record(mine[i])->refcount) == 1);
    }

    return true;
}

static void sd_disjoint_worker(void *arg) {
    size_t tid = (size_t) (uintptr_t) arg;

    sd_disjoint_body(tid);

    for (size_t i = 0; i < sd_mt_disjoint_per_thread; i++) {
        if (sd_disjoint_handles[tid][i]) {
            stack_depot_put(sd_disjoint_handles[tid][i]);
            sd_disjoint_handles[tid][i] = NULL;
        }
    }

    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_disjoint,
                         .group = TEST_GROUP(stack_depot),
                         TEST_INTENSITY(8, 32, 128)) {
    sd_mt_disjoint_per_thread = ctx->intensity_val ? ctx->intensity_val : 32;
    if (sd_mt_disjoint_per_thread > SD_MT_DISJOINT_PER_THREAD_MAX)
        sd_mt_disjoint_per_thread = SD_MT_DISJOINT_PER_THREAD_MAX;

    sd_mt_reset(ctx, SD_MT_THREADS);
    memset(sd_disjoint_handles, 0, sizeof(sd_disjoint_handles));

    sd_mt_spawn("sd_disjoint", sd_disjoint_worker, SD_MT_THREADS);
    SD_MT_JOIN();

    for (size_t t = 0; t < SD_MT_THREADS; t++) {
        for (size_t i = 0; i < sd_mt_disjoint_per_thread; i++) {
            uintptr_t trace[SD_TRACE_LEN];
            sd_make_trace(trace, SD_TRACE_LEN,
                          SD_MT_DISJOINT_ID + t * sd_mt_disjoint_per_thread +
                              i);
            TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
        }
    }

    return TEST_SUCCESS;
}

#define SD_MT_CHURN_SET 4
#define SD_MT_CHURN_ID 0x6000
static size_t sd_mt_churn_iters_count = 4000;

/* Tiny id set and save/put pair keeps refcounts around zero,
 * so we use that to test for races and issues here */
static bool sd_churn_body(size_t tid) {
    uint64_t rng = sd_rng_seed(tid + SD_MT_THREADS);

    for (size_t i = 0; i < sd_mt_churn_iters_count; i++) {
        if (atomic_load(&sd_mt.stop))
            return false;

        size_t id = sd_rng(&rng) % SD_MT_CHURN_SET;
        uintptr_t want[SD_TRACE_LEN];
        sd_make_trace(want, SD_TRACE_LEN, SD_MT_CHURN_ID + id);

        stack_handle_t h =
            stack_depot_save(want, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!h) {
            sd_mt_report_oom();
            return false;
        }

        uintptr_t got[STACK_TRACE_MAX_DEPTH];
        SD_WORKER_CHECK(stack_depot_read(h, got) == SD_TRACE_LEN);
        SD_WORKER_CHECK(!memcmp(got, want, sizeof(want)));
        SD_WORKER_CHECK(refcount_read(&stack_depot_get_record(h)->refcount) >
                        0);

        stack_depot_put(h);
    }

    return true;
}

static void sd_churn_worker(void *arg) {
    sd_churn_body((size_t) (uintptr_t) arg);
    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_churn_race,
                         .group = TEST_GROUP(stack_depot),
                         TEST_INTENSITY(500, 4000, 20000)) {
    sd_mt_churn_iters_count = ctx->intensity_val ? ctx->intensity_val : 4000;
    sd_mt_reset(ctx, SD_MT_THREADS);
    sd_mt_spawn("sd_churn", sd_churn_worker, SD_MT_THREADS);
    SD_MT_JOIN();

    for (size_t id = 0; id < SD_MT_CHURN_SET; id++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, SD_MT_CHURN_ID + id);
        TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
    }

    return TEST_SUCCESS;
}

static stack_handle_t sd_cur_handles[SD_MT_THREADS];
static uintptr_t sd_cur_traces[SD_MT_THREADS][STACK_TRACE_MAX_DEPTH];
static size_t sd_cur_lens[SD_MT_THREADS];

static __noinline bool sd_cur_body(size_t tid) {
    /* Both saves have to come from a single calls site, that's why we 
     * need to loop in sd_save_n(), otherwise it would be two different */
    stack_handle_t h[2] = {0};
    volatile size_t n = 2;

    sd_save_n(h, n);

    stack_handle_t a = h[0], b = h[1];
    if (!a || !b) {
        if (a)
            stack_depot_put(a);
        if (b)
            stack_depot_put(b);
        sd_mt_report_oom();
        return false;
    }

    bool ok = true;
    if (b != a) {
        sd_mt_report("save_current did not dedup within a thread");
        ok = false;
    } else if (refcount_read(&stack_depot_get_record(a)->refcount) < 2) {
        sd_mt_report("save_current dropped a reference");
        ok = false;
    }

    stack_depot_put(b);

    if (ok) {
        sd_cur_lens[tid] = stack_depot_read(a, sd_cur_traces[tid]);
        sd_cur_handles[tid] = a;
        return true;
    }

    stack_depot_put(a);
    return false;
}

static void sd_cur_worker(void *arg) {
    sd_cur_body((size_t) (uintptr_t) arg);
    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_save_current,
                         .group = TEST_GROUP(stack_depot)) {
    sd_mt_reset(ctx, SD_MT_THREADS);
    memset(sd_cur_handles, 0, sizeof(sd_cur_handles));
    memset(sd_cur_lens, 0, sizeof(sd_cur_lens));

    sd_mt_spawn("sd_cur", sd_cur_worker, SD_MT_THREADS);
    SD_MT_JOIN();

    for (size_t i = 0; i < SD_MT_THREADS; i++) {
        TEST_ASSERT(sd_cur_handles[i]);
        TEST_ASSERT(sd_cur_lens[i] > 0);
        TEST_ASSERT(sd_cur_lens[i] <= STACK_TRACE_MAX_DEPTH);

        for (size_t j = i + 1; j < SD_MT_THREADS; j++) {
            bool same_trace = sd_cur_lens[i] == sd_cur_lens[j] &&
                              !memcmp(sd_cur_traces[i], sd_cur_traces[j],
                                      sd_cur_lens[i] * sizeof(uintptr_t));
            TEST_ASSERT(same_trace == (sd_cur_handles[i] == sd_cur_handles[j]));
        }
    }

    for (size_t i = 0; i < SD_MT_THREADS; i++)
        stack_depot_put(sd_cur_handles[i]);

    return TEST_SUCCESS;
}
#endif
