#include "tests/test_internal.h"

#ifdef TEST_STACK_DEPOT
TEST_GROUP_DECLARE(stack_depot, .intensity_desc = {
                                    .curve = SCALE_PIECEWISE_LOG,
                                    .unit = "records",
                                });

#define SD_SEED 0xDEADBEEFULL
#define SD_TRACE_LEN 8
#define SD_MANY 4096
static_assert(SD_MANY > STACK_DEPOT_HASH_SIZE); /* Force collisions */

static void sd_make_trace(uintptr_t *entries, size_t len, uint64_t id) {
    for (size_t i = 0; i < len; i++)
        entries[i] = (uintptr_t) (0xffffffff80000000ULL + (id << 20) + i * 16);
}

TEST_DECLARE_UNIT(stack_depot, basic) {
    stack_handle_t handle = stack_depot_save_current();
    TEST_ASSERT_NONNULL(handle);

    struct stack_depot_record *rec = stack_depot_get_record(handle);
    TEST_ASSERT_NONNULL(rec);
    TEST_ASSERT_GT(rec->num_entries, 0);
    TEST_ASSERT_LE(rec->num_entries, STACK_TRACE_MAX_DEPTH);
    TEST_ASSERT_EQ(refcount_read(&rec->refcount), 1);

    uintptr_t entries[STACK_TRACE_MAX_DEPTH] = {0};
    size_t n = stack_depot_read(handle, entries);
    TEST_ASSERT_EQ(n, rec->num_entries);
    TEST_ASSERT_MEM_EQ(entries, rec->entries, n * sizeof(uintptr_t));

    stack_depot_put(handle);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot, dedup) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, 1);

    stack_handle_t a =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_NONNULL(a);
    TEST_ASSERT_EQ(refcount_read(&stack_depot_get_record(a)->refcount), 1);

    stack_handle_t b =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_PTR_EQ(b, a);
    TEST_ASSERT_EQ(refcount_read(&stack_depot_get_record(a)->refcount), 2);

    /* A copy of the same bytes in a different buffer must dedup */
    uintptr_t copy[SD_TRACE_LEN];
    memcpy(copy, trace, sizeof(copy));
    stack_handle_t c =
        stack_depot_save(copy, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_PTR_EQ(c, a);
    TEST_ASSERT_EQ(refcount_read(&stack_depot_get_record(a)->refcount), 3);

    stack_depot_put(c);
    stack_depot_put(b);
    TEST_ASSERT_EQ(refcount_read(&stack_depot_get_record(a)->refcount), 1);
    stack_depot_put(a);

    stack_handle_t d =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_NONNULL(d);
    TEST_ASSERT_EQ(refcount_read(&stack_depot_get_record(d)->refcount), 1);
    stack_depot_put(d);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot, distinct) {
    uintptr_t a[SD_TRACE_LEN], b[SD_TRACE_LEN];
    sd_make_trace(a, SD_TRACE_LEN, 2);
    sd_make_trace(b, SD_TRACE_LEN, 3);

    stack_handle_t ha = stack_depot_save(a, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    stack_handle_t hb = stack_depot_save(b, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_NONNULL(ha);
    TEST_ASSERT_NONNULL(hb);
    TEST_ASSERT_PTR_NE(ha, hb);

    /* Same prefix at shorter length is different record */
    stack_handle_t hp =
        stack_depot_save(a, SD_TRACE_LEN / 2, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_NONNULL(hp);
    TEST_ASSERT_PTR_NE(hp, ha);
    TEST_ASSERT_EQ(stack_depot_get_record(hp)->num_entries, SD_TRACE_LEN / 2);

    uintptr_t tail[SD_TRACE_LEN];
    memcpy(tail, a, sizeof(tail));
    tail[SD_TRACE_LEN - 1] ^= 0x1000;
    stack_handle_t ht =
        stack_depot_save(tail, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_NONNULL(ht);
    TEST_ASSERT_PTR_NE(ht, ha);

    uintptr_t out[STACK_TRACE_MAX_DEPTH] = {0};
    TEST_ASSERT_EQ(stack_depot_read(ht, out), SD_TRACE_LEN);
    TEST_ASSERT_MEM_EQ(out, tail, sizeof(tail));

    stack_depot_put(ht);
    stack_depot_put(hp);
    stack_depot_put(hb);
    stack_depot_put(ha);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot, hash_bucket) {
    uintptr_t trace[SD_TRACE_LEN];
    sd_make_trace(trace, SD_TRACE_LEN, 4);

    uint32_t expect = stack_depot_hash(trace, SD_TRACE_LEN);
    stack_handle_t h =
        stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT_NONNULL(h);

    struct stack_depot_record *rec = stack_depot_get_record(h);
    TEST_ASSERT_EQ(rec->hash, expect);

    /* Test reachability from our end */
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

TEST_DECLARE_UNIT(stack_depot, many, TEST_INTENSITY(128, 1024, 4096)) {
    size_t count = ctx->intensity_val ? ctx->intensity_val : SD_MANY;
    stack_handle_t *handles =
        kmalloc(sizeof(*handles) * count, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(handles);

    prng_seed(SD_SEED);

    for (size_t i = 0; i < count; i++) {
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, 0x100 + i);
        handles[i] = stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
        if (!handles[i]) {
            /* OOM? Unwind + skip */
            for (size_t j = 0; j < i; j++)
                stack_depot_put(handles[j]);
            kfree(handles);
            return TEST_SKIP(TEST_SKIP_RAM_LOW);
        }
    }

    for (size_t i = 0; i < count; i++) {
        uintptr_t want[SD_TRACE_LEN], got[STACK_TRACE_MAX_DEPTH] = {0};
        sd_make_trace(want, SD_TRACE_LEN, 0x100 + i);

        TEST_ASSERT_EQ(stack_depot_read(handles[i], got), SD_TRACE_LEN);
        TEST_ASSERT_MEM_EQ(got, want, sizeof(want));
        TEST_ASSERT_EQ(
            refcount_read(&stack_depot_get_record(handles[i])->refcount), 1);

        /* Re-saving in random order should hit existing record */
        TEST_ASSERT_PTR_EQ(
            stack_depot_save(want, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT),
            handles[i]);
        stack_depot_put(handles[i]);
    }

    for (size_t i = 0; i < count; i++)
        for (size_t j = i + 1; j < count; j++)
            TEST_ASSERT_PTR_NE(handles[i], handles[j]);

    for (size_t i = 0; i < count; i++)
        stack_depot_put(handles[i]);

    kfree(handles);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(stack_depot, churn, TEST_INTENSITY(200, 2000, 20000)) {
    prng_seed(SD_SEED + 1);

    enum { SD_CHURN_SET = 32 };
    size_t ops = ctx->intensity_val ? ctx->intensity_val : 2000;
    stack_handle_t live[SD_CHURN_SET] = {0};

    for (size_t op = 0; op < ops; op++) {
        size_t i = prng_next() % SD_CHURN_SET;
        uintptr_t trace[SD_TRACE_LEN];
        sd_make_trace(trace, SD_TRACE_LEN, 0x2000 + i);

        if (live[i]) {
            uintptr_t got[STACK_TRACE_MAX_DEPTH] = {0};
            TEST_ASSERT_EQ(stack_depot_read(live[i], got), SD_TRACE_LEN);
            TEST_ASSERT_MEM_EQ(got, trace, sizeof(trace));
            stack_depot_put(live[i]);
            live[i] = NULL;
        } else {
            live[i] =
                stack_depot_save(trace, SD_TRACE_LEN, ALLOC_FLAGS_DEFAULT);
            TEST_ASSERT_NONNULL(live[i]);
            TEST_ASSERT_EQ(stack_depot_get_record(live[i])->num_entries,
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

TEST_DECLARE_UNIT(stack_depot, save_current_dedup) {
    stack_handle_t h[2] = {0};
    volatile size_t n = 2;

    sd_save_n(h, n);
    TEST_ASSERT_NONNULL(h[0]);
    TEST_ASSERT_NONNULL(h[1]);
    TEST_ASSERT_PTR_EQ(h[0], h[1]);
    TEST_ASSERT_EQ(refcount_read(&stack_depot_get_record(h[0])->refcount), 2);

    stack_depot_put(h[1]);
    stack_depot_put(h[0]);
    return TEST_SUCCESS;
}
#endif
