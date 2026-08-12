#ifdef TEST_STACK_DEPOT

#include <crypto/prng.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stack_depot.h>
#include <stdatomic.h>
#include <string.h>
#include <test.h>
#include <thread/thread.h>

TEST_GROUP_DECLARE(stack_depot);

#define SD_SEED 0xDEADBEEFULL
#define SD_TRACE_LEN 8
#define SD_MANY 1024 /* > STACK_DEPOT_HASH_SIZE, forces chain collisions */

static void sd_make_trace(uintptr_t *entries, size_t len, uint64_t id) {
    for (size_t i = 0; i < len; i++)
        entries[i] = (uintptr_t) (0xffffffff80000000ULL + (id << 20) + i * 16);
}

TEST_DECLARE_UNIT(stack_depot_basic, .group = TEST_GROUP(stack_depot)) {
    stack_handle_t handle = stack_depot_save_current();
    TEST_ASSERT(handle);

    struct stack_depot_record *rec = stack_depot_get_record(handle);
    TEST_ASSERT(rec);
    TEST_ASSERT(rec->num_entries > 0);
    TEST_ASSERT(rec->num_entries <= STACK_TRACE_MAX_DEPTH);
    TEST_ASSERT(refcount_read(&rec->refcount) == 1);

    uintptr_t entries[STACK_TRACE_MAX_DEPTH] = {0};
    size_t n = stack_depot_read(handle, entries);
    TEST_ASSERT(n == rec->num_entries);
    TEST_ASSERT(!memcmp(entries, rec->entries, n * sizeof(uintptr_t)));

    stack_depot_put(handle);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot_dedup, .group = TEST_GROUP(stack_depot)) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, 1);

    stack_handle_t a =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(a);
    TEST_ASSERT(refcount_read(&stack_depot_get_record(a)->refcount) == 1);

    stack_handle_t b =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(b == a);
    TEST_ASSERT(refcount_read(&stack_depot_get_record(a)->refcount) == 2);

    /* A copy of the same bytes in a different buffer must still dedup */
    uintptr_t copy[SD_TRACE_LEN];
    memcpy(copy, trace, sizeof(copy));
    stack_handle_t c =
        stack_depot_save(copy, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(c == a);
    TEST_ASSERT(refcount_read(&stack_depot_get_record(a)->refcount) == 3);

    stack_depot_put(c);
    stack_depot_put(b);
    TEST_ASSERT(refcount_read(&stack_depot_get_record(a)->refcount) == 1);
    stack_depot_put(a);

    stack_handle_t d =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(d);
    TEST_ASSERT(refcount_read(&stack_depot_get_record(d)->refcount) == 1);
    stack_depot_put(d);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot_distinct, .group = TEST_GROUP(stack_depot)) {
    uintptr_t a[SD_TRACE_LEN], b[SD_TRACE_LEN];
    sd_make_trace(a, SD_TRACE_LEN, 2);
    sd_make_trace(b, SD_TRACE_LEN, 3);

    stack_handle_t ha = stack_depot_save(a, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    stack_handle_t hb = stack_depot_save(b, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(ha && hb);
    TEST_ASSERT(ha != hb);

    /* Same prefix, shorter length -> different record. */
    stack_handle_t hp =
        stack_depot_save(a, SD_TRACE_LEN / 2, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(hp && hp != ha);
    TEST_ASSERT(stack_depot_get_record(hp)->num_entries == SD_TRACE_LEN / 2);

    uintptr_t tail[SD_TRACE_LEN];
    memcpy(tail, a, sizeof(tail));
    tail[SD_TRACE_LEN - 1] ^= 0x1000;
    stack_handle_t ht =
        stack_depot_save(tail, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(ht && ht != ha);

    uintptr_t out[STACK_TRACE_MAX_DEPTH] = {0};
    TEST_ASSERT(stack_depot_read(ht, out) == SD_TRACE_LEN);
    TEST_ASSERT(!memcmp(out, tail, sizeof(tail)));

    stack_depot_put(ht);
    stack_depot_put(hp);
    stack_depot_put(hb);
    stack_depot_put(ha);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot_hash_bucket, .group = TEST_GROUP(stack_depot)) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, 4);

    uint32_t expect = stack_depot_hash(trace, SD_TRACE_LEN);
    stack_handle_t h =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(h);

    struct stack_depot_record *rec = stack_depot_get_record(h);
    TEST_ASSERT(rec->hash == expect);

    /* The record is actually reachable on that chain. */
    struct stack_depot_record_chain *chain =
        &stack_depot_global.chains[rec->hash % STACK_DEPOT_HASH_SIZE];
    bool found = false;
    struct stack_depot_record *pos;
    enum irql irql = spin_lock(&chain->lock);
    list_for_each_entry(pos, &chain->list, hash_list) {
        if (pos == rec) {
            found = true;
            break;
        }
    }
    spin_unlock(&chain->lock, irql);
    TEST_ASSERT(found);

    stack_depot_put(h);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot_many, .group = TEST_GROUP(stack_depot)) {
    stack_handle_t *handles =
        kmalloc(sizeof(*handles) * SD_MANY, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(handles);

    prng_seed(SD_SEED);

    for (size_t i = 0; i < SD_MANY; i++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, 0x100 + i);
        handles[i] = stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!handles[i]) {
            /* Out of record memory - unwind what we took and skip. */
            for (size_t j = 0; j < i; j++)
                stack_depot_put(handles[j]);
            kfree(handles);
            return TEST_SKIP(TEST_SKIP_RAM_LOW);
        }
    }

    for (size_t i = 0; i < SD_MANY; i++) {
        uintptr_t want[SD_TRACE_LEN], got[STACK_TRACE_MAX_DEPTH] = {0};
        sd_make_trace(want, SD_TRACE_LEN, 0x100 + i);

        TEST_ASSERT(stack_depot_read(handles[i], got) == SD_TRACE_LEN);
        TEST_ASSERT(!memcmp(got, want, sizeof(want)));
        TEST_ASSERT(
            refcount_read(&stack_depot_get_record(handles[i])->refcount) == 1);

        /* Re-saving in random order must hit the existing record. */
        TEST_ASSERT(stack_depot_save(want, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT) ==
                    handles[i]);
        stack_depot_put(handles[i]);
    }

    for (size_t i = 0; i < SD_MANY; i++)
        for (size_t j = i + 1; j < SD_MANY; j++)
            TEST_ASSERT(handles[i] != handles[j]);

    for (size_t i = 0; i < SD_MANY; i++)
        stack_depot_put(handles[i]);

    kfree(handles);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot_churn, .group = TEST_GROUP(stack_depot)) {
    prng_seed(SD_SEED + 1);

    enum { SD_CHURN_SET = 32, SD_CHURN_OPS = 2000 };
    stack_handle_t live[SD_CHURN_SET] = {0};

    for (size_t op = 0; op < SD_CHURN_OPS; op++) {
        size_t i = prng_next() % SD_CHURN_SET;
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, 0x2000 + i);

        if (live[i]) {
            uintptr_t got[STACK_TRACE_MAX_DEPTH] = {0};
            TEST_ASSERT(stack_depot_read(live[i], got) == SD_TRACE_LEN);
            TEST_ASSERT(!memcmp(got, trace, sizeof(trace)));
            stack_depot_put(live[i]);
            live[i] = NULL;
        } else {
            live[i] =
                stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
            TEST_ASSERT(live[i]);
            TEST_ASSERT(stack_depot_get_record(live[i])->num_entries ==
                        SD_TRACE_LEN);
        }
    }

    for (size_t i = 0; i < SD_CHURN_SET; i++)
        if (live[i])
            stack_depot_put(live[i]);

    return TEST_SUCCESS;
}

static __noinline void sd_save_n(stack_handle_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        out[i] = stack_depot_save_current();
}

TEST_DECLARE_UNIT(stack_depot_save_current_dedup,
                  .group = TEST_GROUP(stack_depot)) {
    stack_handle_t h[2] = {0};
    volatile size_t n = 2;

    sd_save_n(h, n);
    TEST_ASSERT(h[0] && h[1]);
    TEST_ASSERT(h[0] == h[1]);
    TEST_ASSERT(refcount_read(&stack_depot_get_record(h[0])->refcount) == 2);

    stack_depot_put(h[1]);
    stack_depot_put(h[0]);
    return TEST_SUCCESS;
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

/* Joins the workers and folds their verdict into ours. */
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

        /* one straggler burns the whole budget, so once we give up on it
         * we stop waiting on the rest too */
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

    /* every worker ran to completion, so the fleet was fully spawned */
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

    /* Raised so the whole fleet is queued before any of it runs. */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < n; i++)
        sd_mt.threads[i] =
            thread_spawn_joinable(name, fn, (void *) (uintptr_t) i);
    irql_lower(irql);
}

#define SD_MT_DEDUP_SAVES 64
#define SD_MT_DEDUP_ID 0x3000

static stack_handle_t sd_dedup_handles[SD_MT_THREADS];
static atomic_uint sd_dedup_saved;
static atomic_bool sd_dedup_release;

static bool sd_dedup_body(size_t tid, stack_handle_t *held, size_t *held_n) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, SD_MT_DEDUP_ID);

    for (size_t i = 0; i < SD_MT_DEDUP_SAVES; i++) {
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
    stack_handle_t held[SD_MT_DEDUP_SAVES];
    size_t held_n = 0;

    sd_dedup_body(tid, held, &held_n);

    /* Publish unconditionally - the main thread's barrier waits on this
     * count even when we bailed out early. */
    atomic_fetch_add(&sd_dedup_saved, 1);

    while (!atomic_load(&sd_dedup_release))
        scheduler_yield();

    for (size_t i = 0; i < held_n; i++)
        stack_depot_put(held[i]);

    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_dedup,
                         .group = TEST_GROUP(stack_depot)) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, SD_MT_DEDUP_ID);
    TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);

    sd_mt_reset(ctx, SD_MT_THREADS);
    atomic_store(&sd_dedup_saved, 0);
    atomic_store(&sd_dedup_release, false);
    memset(sd_dedup_handles, 0, sizeof(sd_dedup_handles));

    sd_mt_spawn("sd_dedup", sd_dedup_worker, SD_MT_THREADS);

    /* All refs are taken and none released yet: the depot must hold
     * exactly one record with every reference accounted for. */
    bool barrier = sd_mt_wait_for(&sd_dedup_saved, SD_MT_THREADS);
    bool clean = !atomic_load(&sd_mt.fail) && !atomic_load(&sd_mt.oom);

    if (barrier && clean) {
        stack_handle_t h = sd_dedup_handles[0];
        if (h) {
            struct stack_depot_record *rec = stack_depot_get_record(h);
            if (refcount_read(&rec->refcount) !=
                SD_MT_THREADS * SD_MT_DEDUP_SAVES)
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

    /* Last put drops the record off its chain. */
    TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
    return TEST_SUCCESS;
}

#define SD_MT_SET 16
#define SD_MT_SET_ITERS 1500
#define SD_MT_SET_ID 0x4000

static bool sd_shared_body(size_t tid) {
    uint64_t rng = sd_rng_seed(tid);

    for (size_t i = 0; i < SD_MT_SET_ITERS; i++) {
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
        /* While we hold a ref the record must stay ours: right content,
         * right length, right bucket, and alive. */
        SD_WORKER_CHECK(rec->num_entries == SD_TRACE_LEN);
        SD_WORKER_CHECK(rec->hash == stack_depot_hash(want, SD_TRACE_LEN));
        SD_WORKER_CHECK(refcount_read(&rec->refcount) > 0);

        uintptr_t got[STACK_TRACE_MAX_DEPTH];
        size_t n = stack_depot_read(h, got);
        SD_WORKER_CHECK(n == SD_TRACE_LEN);
        SD_WORKER_CHECK(!memcmp(got, want, sizeof(want)));

        if (sd_rng(&rng) & 1)
            scheduler_yield();

        /* Re-read after a possible preemption - a racing put must not
         * have recycled the record under our reference. */
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
                         .group = TEST_GROUP(stack_depot)) {
    sd_mt_reset(ctx, SD_MT_THREADS);
    sd_mt_spawn("sd_shared", sd_shared_worker, SD_MT_THREADS);
    SD_MT_JOIN();

    /* Everything was put back; nothing may be left behind. */
    for (size_t id = 0; id < SD_MT_SET; id++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, SD_MT_SET_ID + id);
        TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
    }

    return TEST_SUCCESS;
}

#define SD_MT_DISJOINT_PER_THREAD 32
#define SD_MT_DISJOINT_ID 0x5000

static stack_handle_t sd_disjoint_handles[SD_MT_THREADS]
                                         [SD_MT_DISJOINT_PER_THREAD];

static bool sd_disjoint_body(size_t tid) {
    stack_handle_t *mine = sd_disjoint_handles[tid];

    for (size_t i = 0; i < SD_MT_DISJOINT_PER_THREAD; i++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN,
                      SD_MT_DISJOINT_ID + tid * SD_MT_DISJOINT_PER_THREAD + i);

        mine[i] = stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!mine[i]) {
            sd_mt_report_oom();
            return false;
        }

        /* Nobody else uses this id, so we are the only reference. */
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
    for (size_t i = 0; i < SD_MT_DISJOINT_PER_THREAD; i++) {
        uintptr_t want[SD_TRACE_LEN], got[STACK_TRACE_MAX_DEPTH];
        sd_make_trace(want, SD_TRACE_LEN,
                      SD_MT_DISJOINT_ID + tid * SD_MT_DISJOINT_PER_THREAD + i);

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

    for (size_t i = 0; i < SD_MT_DISJOINT_PER_THREAD; i++) {
        if (sd_disjoint_handles[tid][i]) {
            stack_depot_put(sd_disjoint_handles[tid][i]);
            sd_disjoint_handles[tid][i] = NULL;
        }
    }

    atomic_fetch_sub(&sd_mt.left, 1);
}

TEST_DECLARE_INTEGRATION(stack_depot_mt_disjoint,
                         .group = TEST_GROUP(stack_depot)) {
    sd_mt_reset(ctx, SD_MT_THREADS);
    memset(sd_disjoint_handles, 0, sizeof(sd_disjoint_handles));

    sd_mt_spawn("sd_disjoint", sd_disjoint_worker, SD_MT_THREADS);
    SD_MT_JOIN();

    for (size_t t = 0; t < SD_MT_THREADS; t++) {
        for (size_t i = 0; i < SD_MT_DISJOINT_PER_THREAD; i++) {
            uintptr_t trace[SD_TRACE_LEN];
            sd_make_trace(trace, SD_TRACE_LEN,
                          SD_MT_DISJOINT_ID + t * SD_MT_DISJOINT_PER_THREAD +
                              i);
            TEST_ASSERT(sd_chain_count(trace, SD_TRACE_LEN) == 0);
        }
    }

    return TEST_SUCCESS;
}

#define SD_MT_CHURN_SET 4
#define SD_MT_CHURN_ITERS 4000
#define SD_MT_CHURN_ID 0x6000

/* A tiny id set and a tight save/put pair keep refcounts hovering around
 * zero, which is exactly where save() racing the freeing put() has to
 * either find a live record or build a new one. */
static bool sd_churn_body(size_t tid) {
    uint64_t rng = sd_rng_seed(tid + SD_MT_THREADS);

    for (size_t i = 0; i < SD_MT_CHURN_ITERS; i++) {
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
                         .group = TEST_GROUP(stack_depot)) {
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
    /* Both saves have to issue from a single call site. save_current() unwinds
     * from its own frame, so the caller's return address is part of the trace
     * and two textually distinct calls legitimately hash differently — hence
     * the loop in sd_save_n() rather than two calls spelled out here. */
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
