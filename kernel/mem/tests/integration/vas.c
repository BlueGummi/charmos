#include "mem/tests/test_internal.h"
#include "mem/vas_internal.h"

#define SMP_VAS_BASE 0x710000000000ULL

static struct thread *vas_worker_on(size_t cpu, void (*entry)(void *),
                                    void *arg) {
    return thread_spawn("vas_worker", entry, .arg = arg, .joinable = true,
                        .allowed_cpus = cpu_mask_of(cpu), .pinned = true,
                        .on_cpu = cpu);
}

struct handoff {
    struct vas *vas;
    vaddr_t addr;
    size_t cpu;
    size_t size;
    bool valid;
};

static void handoff_alloc(void *arg) {
    struct handoff *h = arg;
    h->cpu = smp_id(TOPC_NONE);
    h->addr = vas_alloc(h->vas, h->size, PAGE_SIZE);
}

static void handoff_free(void *arg) {
    struct handoff *h = arg;
    h->cpu = smp_id(TOPC_NONE);
    h->valid = vas_vaddr_is_allocated(h->vas, h->addr + h->size - 1);
    vas_free(h->vas, h->addr, h->size);
    h->valid &= !vas_vaddr_is_allocated(h->vas, h->addr);
}

TEST_DEFINE_INTEGRATION(vas, cross_cpu_free_without_any_free_gap,
                        .min_cores = 2) {
    struct handoff h = {
        .vas = vas_create(SMP_VAS_BASE, SMP_VAS_BASE + VAS_CHUNK_SIZE),
        .size = VAS_CHUNK_SIZE,
    };

    TEST_ASSERT_NONNULL(h.vas);
    struct thread *producer = vas_worker_on(0, handoff_alloc, &h);
    TEST_ASSERT_NONNULL(producer);

    thread_join(producer);
    TEST_ASSERT_EQ(h.cpu, 0);
    TEST_ASSERT_EQ(h.addr, SMP_VAS_BASE);
    TEST_ASSERT_EQ(atomic_load(&h.vas->chunk_owner[0]), 0);
    TEST_ASSERT_EQ(h.vas->local[0].total_free, 0);

    struct thread *consumer = vas_worker_on(1, handoff_free, &h);
    TEST_ASSERT_NONNULL(consumer);

    thread_join(consumer);
    TEST_ASSERT_EQ(h.cpu, 1);
    TEST_ASSERT(h.valid);
    TEST_ASSERT_EQ(h.vas->local[0].total_free, VAS_CHUNK_SIZE);
    TEST_ASSERT_EQ(h.vas->local[1].total_free, 0);
    TEST_ASSERT(vas_destroy(h.vas));

    return TEST_SUCCESS;
}

struct churn_worker {
    struct vas *vas;
    size_t expected_cpu;
    bool passed;
};

static void concurrent_churn(void *arg) {
    struct churn_worker *w = arg;
    w->passed = smp_id(TOPC_NONE) == w->expected_cpu;
    uint64_t random = 0x123456789ULL + w->expected_cpu;
    for (size_t i = 0; i < 600; i++) {
        const size_t classes[] = {PAGE_SIZE, 5 * PAGE_SIZE, 17 * PAGE_SIZE,
                                  PAGE_2MB};
        size_t size =
            i % 2 ? classes[(i / 2) % TEST_ARRAY_LEN(classes)]
                  : (1 + prng_splitmix64_next(&random) % 512) * PAGE_SIZE;
        size_t align = i % 3 ? PAGE_SIZE : PAGE_2MB;
        vaddr_t addr = vas_alloc(w->vas, size, align);
        if (!addr) {
            w->passed = false;
            break;
        }
        w->passed &= vas_vaddr_is_allocated(w->vas, addr + size - 1);
        if (i % 7 == 0)
            vas_reclaim(w->vas);
        vas_free(w->vas, addr, size);
    }
}

TEST_DEFINE_INTEGRATION(vas, mt_import_query_and_reclaim, .min_cores = 2) {
    struct vas *vas =
        vas_create(SMP_VAS_BASE, SMP_VAS_BASE + 8 * VAS_CHUNK_SIZE);

    struct thread *threads[4] = {0};
    struct churn_worker workers[4] = {0};
    size_t thread_capacity = sizeof(threads) / sizeof(*threads);
    size_t count = MIN(global.core_count, thread_capacity);

    TEST_ASSERT_NONNULL(vas);
    size_t started = 0;

    for (; started < count; started++) {
        workers[started].vas = vas;
        workers[started].expected_cpu = started;
        threads[started] =
            vas_worker_on(started, concurrent_churn, &workers[started]);

        if (!threads[started])
            break;
    }

    bool passed = started == count;
    for (size_t i = 0; i < started; i++) {
        thread_join(threads[i]);
        passed &= workers[i].passed;
    }

    vas_reclaim(vas);
    TEST_ASSERT_EQ(vas->global.total_free, vas->limit - vas->base);
    TEST_ASSERT(vas_destroy(vas));
    TEST_ASSERT(passed);
    return TEST_SUCCESS;
}

static bool vas_run_on(size_t cpu, void (*entry)(void *), void *arg) {
    struct thread *thread = vas_worker_on(cpu, entry, arg);
    if (!thread)
        return false;

    thread_join(thread);
    return true;
}

TEST_DEFINE_INTEGRATION(vas, magazine_remote_free_does_not_cache,
                        .min_cores = 2) {
    struct handoff h = {
        .vas = vas_create(SMP_VAS_BASE, SMP_VAS_BASE + VAS_CHUNK_SIZE),
        .size = PAGE_SIZE,
    };

    TEST_ASSERT_NONNULL(h.vas);
    TEST_ASSERT(vas_run_on(0, handoff_alloc, &h));
    TEST_ASSERT_NE(h.addr, 0);
    size_t enrolled = atomic_load(&h.vas->mag_reserved_bytes);
    TEST_ASSERT_GE(enrolled, PAGE_SIZE);
    TEST_ASSERT(vas_run_on(1, handoff_free, &h));
    TEST_ASSERT(h.valid);
    TEST_ASSERT_EQ(atomic_load(&h.vas->mag_reserved_bytes),
                   enrolled - PAGE_SIZE);

    TEST_ASSERT_EQ(h.vas->local[1].mag_free_hits, 0);
    TEST_ASSERT_EQ(h.vas->local[0].mag_free_hits, 0);
    TEST_ASSERT(vas_destroy(h.vas));
    return TEST_SUCCESS;
}

/* explicit phase boundaries to verify remote visibility */
struct magazine_visibility {
    struct vas *vas;
    vaddr_t addr;
    atomic_uint32_t phase;
    bool valid;
};

static bool await_phase(struct magazine_visibility *v, uint32_t phase) {
    while (true) {
        uint32_t current = atomic_load_acq(&v->phase);
        if (current == UINT32_MAX)
            return false;
        if (current == phase)
            return true;
        scheduler_yield();
    }
}

static void visibility_owner(void *arg) {
    struct magazine_visibility *v = arg;
    for (uint32_t i = 0; i < 200; i++) {
        v->addr = vas_alloc(v->vas, PAGE_SIZE, PAGE_SIZE);
        if (!v->addr) {
            atomic_store(&v->phase, UINT32_MAX);
            return;
        }
        atomic_store_release(&v->phase, 1);
        if (!await_phase(v, 2)) {
            vas_free(v->vas, v->addr, PAGE_SIZE);
            return;
        }
        vas_free(v->vas, v->addr, PAGE_SIZE);
        atomic_store_release(&v->phase, 3);
        if (!await_phase(v, 4))
            return;
    }
}

static void visibility_observer(void *arg) {
    struct magazine_visibility *v = arg;
    v->valid = true;
    for (uint32_t i = 0; i < 200; i++) {
        if (!await_phase(v, 1)) {
            v->valid = false;
            return;
        }
        vas_reclaim(v->vas); /* Must keep the owner's live slot intact */
        v->valid &= vas_vaddr_is_allocated(v->vas, v->addr + PAGE_SIZE - 1);
        atomic_store_release(&v->phase, 2);
        if (!await_phase(v, 3)) {
            v->valid = false;
            return;
        }
        v->valid &= !vas_vaddr_is_allocated(v->vas, v->addr);
        vas_reclaim(v->vas);
        v->valid &= atomic_load(&v->vas->mag_reserved_bytes) == 0;
        atomic_store_release(&v->phase, 4);
    }
}

TEST_DEFINE_INTEGRATION(vas, magazine_remote_visibility_and_drain,
                        .min_cores = 2) {
    struct magazine_visibility v = {
        .vas = vas_create(SMP_VAS_BASE, SMP_VAS_BASE + VAS_CHUNK_SIZE),
    };
    TEST_ASSERT_NONNULL(v.vas);
    atomic_init(&v.phase, 0);
    struct thread *owner = vas_worker_on(0, visibility_owner, &v);
    TEST_ASSERT_NONNULL(owner);
    struct thread *observer = vas_worker_on(1, visibility_observer, &v);
    if (!observer)
        atomic_store(&v.phase, UINT32_MAX);
    thread_join(owner);
    if (observer)
        thread_join(observer);
    TEST_ASSERT(vas_destroy(v.vas));
    TEST_ASSERT(v.valid);
    return TEST_SUCCESS;
}

struct budget_worker {
    struct vas *vas;
    vaddr_t addresses[2];
};

static void budget_alloc(void *arg) {
    struct budget_worker *w = arg;
    for (size_t i = 0; i < TEST_ARRAY_LEN(w->addresses); i++)
        w->addresses[i] = vas_alloc(w->vas, PAGE_2MB, PAGE_2MB);
}

static void budget_free(void *arg) {
    struct budget_worker *w = arg;
    for (size_t i = 0; i < TEST_ARRAY_LEN(w->addresses); i++)
        if (w->addresses[i])
            vas_free(w->vas, w->addresses[i], PAGE_2MB);
}

TEST_DEFINE_INTEGRATION(vas, magazine_per_vas_byte_budget, .min_cores = 2) {
    struct vas *vas =
        vas_create(SMP_VAS_BASE, SMP_VAS_BASE + 4 * VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    struct budget_worker workers[2] = {{.vas = vas}, {.vas = vas}};
    for (uint32_t cpu = 0; cpu < 2; cpu++) {
        TEST_ASSERT(vas_run_on(cpu, budget_alloc, &workers[cpu]));
        for (size_t i = 0; i < TEST_ARRAY_LEN(workers[cpu].addresses); i++)
            TEST_ASSERT_NE(workers[cpu].addresses[i], 0);
    }
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes), VAS_MAG_BYTE_LIMIT);
    struct handoff extra = {.vas = vas, .size = PAGE_SIZE};
    TEST_ASSERT(vas_run_on(0, handoff_alloc, &extra));
    TEST_ASSERT_NE(extra.addr, 0); /* Budget denies caching */
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes), VAS_MAG_BYTE_LIMIT);
    TEST_ASSERT(vas_run_on(0, handoff_free, &extra));
    TEST_ASSERT_EQ(vas->local[0].mag_free_hits, 0);
    for (uint32_t cpu = 0; cpu < 2; cpu++)
        TEST_ASSERT(vas_run_on(cpu, budget_free, &workers[cpu]));
    vas_reclaim(vas);
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes), 0);
    TEST_ASSERT_EQ(vas->global.total_free, vas->limit - vas->base);
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}
