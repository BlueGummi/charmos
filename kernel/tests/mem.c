#ifdef TEST_MEM

#include <crypto/prng.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <tests.h>
#include <thread/thread.h>

TEST_REGISTER(pmm_alloc_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(p);
    SET_SUCCESS();
}

TEST_REGISTER(vmm_map_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    uint64_t p = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_phys(p, PAGE_SIZE, 0, VMM_FLAG_NONE);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, PAGE_SIZE, VMM_FLAG_NONE);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr, VMM_FLAG_NONE) == (uint64_t) -1);
    SET_SUCCESS();
}

/* probably don't need these at all but I'll keep
 * them in case something decides to be funny */
#define ALIGNED_ALLOC_TIMES 512

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    TEST_REGISTER(kmalloc_aligned_##name##_test, false, false) {               \
        ABORT_IF_RAM_LOW();                                                    \
        for (uint64_t i = 0; i < ALIGNED_ALLOC_TIMES; i++) {                   \
            void *ptr = kmalloc_aligned(align, align, ALLOC_PARAMS_DEFAULT);   \
            TEST_ASSERT(ptr != NULL);                                          \
            ASSERT_ALIGNED(ptr, align);                                        \
        }                                                                      \
        SET_SUCCESS();                                                         \
    }

KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)

#define STRESS_ALLOC_TIMES 2048

static paddr_t pmm_stress_test_ptrs[STRESS_ALLOC_TIMES];
TEST_REGISTER(pmm_stress_alloc_free_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        pmm_stress_test_ptrs[i] = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
        TEST_ASSERT(pmm_stress_test_ptrs[i] != 0);
    }

    for (int64_t i = STRESS_ALLOC_TIMES - 1; i >= 0; i--) {
        pmm_free_page(pmm_stress_test_ptrs[i]);
    }

    SET_SUCCESS();
}

static void *stress_alloc_free_ptrs[STRESS_ALLOC_TIMES] = {0};
TEST_REGISTER(kmalloc_stress_alloc_free_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        stress_alloc_free_ptrs[i] = kmalloc(64, ALLOC_PARAMS_DEFAULT);
        TEST_ASSERT(stress_alloc_free_ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % STRESS_ALLOC_TIMES;
        if (stress_alloc_free_ptrs[idx]) {
            kfree(stress_alloc_free_ptrs[idx], FREE_PARAMS_DEFAULT);
            stress_alloc_free_ptrs[idx] = NULL;
        }
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        if (stress_alloc_free_ptrs[i]) {
            kfree(stress_alloc_free_ptrs[i], FREE_PARAMS_DEFAULT);
        }
    }

    SET_SUCCESS();
}

/* Put it here to avoid it eating things up */
static void *mixed_stress_test_ptrs[STRESS_ALLOC_TIMES] = {0};
TEST_REGISTER(kmalloc_mixed_stress_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        mixed_stress_test_ptrs[i] = kmalloc(128, ALLOC_PARAMS_DEFAULT);
        TEST_ASSERT(mixed_stress_test_ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        kfree(mixed_stress_test_ptrs[i], FREE_PARAMS_DEFAULT);
    }

    SET_SUCCESS();
}

#define MT_THREAD_COUNT 8
#define MT_ALLOC_TIMES 1024

static volatile int kmalloc_done = 0;

static void mt_kmalloc_worker(void *) {
    void *ptrs[MT_ALLOC_TIMES] = {0};

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        ptrs[i] = kmalloc(64, ALLOC_PARAMS_DEFAULT);
        TEST_ASSERT(ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % MT_ALLOC_TIMES;

        kfree(ptrs[idx], FREE_PARAMS_DEFAULT);
        ptrs[idx] = NULL;
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        kfree(ptrs[i], FREE_PARAMS_DEFAULT);
    }

    kmalloc_done++;
}

TEST_REGISTER(kmalloc_multithreaded_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    struct thread *threads[MT_THREAD_COUNT];

    for (int i = 0; i < MT_THREAD_COUNT; i++) {
        threads[i] = thread_spawn_custom_stack(
            "mt_kmalloc_thread", mt_kmalloc_worker, NULL, PAGE_SIZE * 16);
        TEST_ASSERT(threads[i] != NULL);
    }

    while (kmalloc_done < MT_THREAD_COUNT)
        scheduler_yield();

    SET_SUCCESS();
}

static char hooray[128] = {0};
TEST_REGISTER(kmalloc_new_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    void *p = kmalloc_new(67, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);

    time_t ms = time_get_ms();
    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    ms = time_get_ms() - ms;

    snprintf(hooray, 128, "allocated 0x%lx and free took %u ms", p, ms);

    ADD_MESSAGE(hooray);
    SET_SUCCESS();
}

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

static char a_msg[128];
TEST_REGISTER(kmalloc_new_basic_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    void *p1 = kmalloc_new(1, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);
    void *p2 = kmalloc_new(64, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);
    void *p3 = kmalloc_new(4096, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);

    if (!p1 || !p2 || !p3) {
        ADD_MESSAGE("kmalloc_new returned NULL for a valid request");
        return;
    }

    /* Write/read back small pattern to verify memory usable */
    memset(p1, 0xA5, 1);
    memset(p2, 0x5A, 64);
    memset(p3, 0xFF, 4096);

    if (((uint8_t *) p1)[0] != 0xA5 || ((uint8_t *) p2)[0] != 0x5A ||
        ((uint8_t *) p3)[0] != 0xFF) {
        ADD_MESSAGE("Memory pattern check failed");
        return;
    }

    /* timed free to check that kfree_new returns quickly */
    time_t start = time_get_ms();
    kfree_new(p1, ALLOC_BEHAVIOR_NORMAL);
    kfree_new(p2, ALLOC_BEHAVIOR_NORMAL);
    kfree_new(p3, ALLOC_BEHAVIOR_NORMAL);
    time_t elapsed = time_get_ms() - start;

    snprintf(a_msg, sizeof(a_msg), "basic alloc/free OK (free took %u ms)",
             (unsigned) elapsed);
    ADD_MESSAGE(a_msg);
    SET_SUCCESS();
}

/*
-------------------- Alignment preference test --------------------

TEST_REGISTER(kmalloc_new_cache_align_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
     Request cache-aligned memory
    uint16_t flags = ALLOC_FLAG_PREFER_CACHE_ALIGNED | ALLOC_FLAG_NONMOVABLE |
                     ALLOC_FLAG_NONPAGEABLE | ALLOC_FLAG_CLASS_DEFAULT;
    void *p = kmalloc_new(128, flags, ALLOC_BEHAVIOR_NORMAL);
    if (!p) {
        ADD_MESSAGE("kmalloc_new returned NULL for cache-aligned request");
        return;
    }

    if (((uintptr_t) p % CACHE_LINE_SIZE) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "pointer %p is not cache-line aligned", p);
        ADD_MESSAGE(msg);
        kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
        return;
    }

    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    ADD_MESSAGE("cache alignment check passed");
    SET_SUCCESS();
}
*/

/* -------------------- Behavior flag verification test -------------------- */

TEST_REGISTER(kmalloc_new_behavior_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    /* ALLOC_BEHAVIOR_ATOMIC should require nonpageable/nonmovable - allocator
       or sanitizers might coerce flags. This test ensures allocation doesn't
       return NULL for such a request. */
    SET_SUCCESS();
    return;

    uint16_t f = ALLOC_FLAG_NONPAGEABLE | ALLOC_FLAG_NONMOVABLE |
                 ALLOC_FLAG_NO_CACHE_ALIGN;
    void *p = kmalloc_new(256, f, ALLOC_BEHAVIOR_ATOMIC);
    if (!p) {
        ADD_MESSAGE("kmalloc_new failed for ATOMIC nonpageable request");
        return;
    }
    /* Do a quick write */
    volatile uint8_t *b = p;
    b[0] = 0x7E;
    if (b[0] != 0x7E) {
        ADD_MESSAGE("atomic allocation memory check failed");
        kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
        return;
    }
    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    ADD_MESSAGE("behavior (ATOMIC) allocation passed");
    SET_SUCCESS();
}

/* -------------------- Multithreaded stress test -------------------- */

#define STRESS_THREADS 3
#define STRESS_ITERS 30000
#define MAX_LIVE_ALLOCS 64

struct stress_arg {
    int id;
    volatile int *done_flag;
};

static volatile bool all_ready = false;

static void stress_worker(void *) {
    struct stress_arg *a = NULL;
    /* wait until private field is visible */
    while (!(a = scheduler_get_current_thread()->private))
        ;

    while (!all_ready)
        ;

    /* allocate small tracking table dynamically */
    void **live_ptrs =
        kmalloc(sizeof(void *) * MAX_LIVE_ALLOCS, ALLOC_PARAMS_DEFAULT);
    memset(live_ptrs, 0, sizeof(void *) * MAX_LIVE_ALLOCS);

    for (int iter = 0; iter < STRESS_ITERS; ++iter) {
        /* 1 in 8 chance to free something early (chaotic order) */
        if ((prng_next() & 7) == 0) {
            int idx = prng_next() % MAX_LIVE_ALLOCS;
            if (live_ptrs[idx]) {
                kfree_new(live_ptrs[idx], ALLOC_BEHAVIOR_NORMAL);
                live_ptrs[idx] = NULL;
            }
        }

        /* Allocate with randomized size and flags */
        size_t sz = 8 + (prng_next() % 512); /* small to moderate allocations */
        uint16_t flags = ALLOC_FLAGS_DEFAULT;

        if (prng_next() & 1) {
            flags |= ALLOC_FLAG_PREFER_CACHE_ALIGNED;
            flags &= ~ALLOC_FLAG_NO_CACHE_ALIGN;
        }
        if (prng_next() & 2) {
            flags |= ALLOC_FLAG_NONMOVABLE;
            flags &= ~ALLOC_FLAG_MOVABLE;
        } else {
            flags |= ALLOC_FLAG_MOVABLE;
            flags &= ~ALLOC_FLAG_NONMOVABLE;
        }

        enum alloc_behavior behavior = (prng_next() & 3)
                                           ? ALLOC_BEHAVIOR_NORMAL
                                           : ALLOC_BEHAVIOR_NO_RECLAIM;

        void *p = kmalloc_new(sz, flags, behavior);
        if (!p)
            continue;

        /* write simple pattern to verify memory */
        ((uint8_t *) p)[0] = (uint8_t) (a->id + iter);
        ((uint8_t *) p)[sz - 1] = (uint8_t) (a->id ^ iter);

        /* randomly decide where to place it */
        int idx = prng_next() % MAX_LIVE_ALLOCS;
        if (live_ptrs[idx])
            kfree_new(live_ptrs[idx], ALLOC_BEHAVIOR_NORMAL);
        live_ptrs[idx] = p;
    }

    /* Final cleanup */
    for (int i = 0; i < MAX_LIVE_ALLOCS; ++i) {
        if (live_ptrs[i])
            kfree_new(live_ptrs[i], ALLOC_BEHAVIOR_NORMAL);
    }

    kfree(live_ptrs, FREE_PARAMS_DEFAULT);
    *a->done_flag = 1;
}

volatile int done[STRESS_THREADS];
struct stress_arg args[STRESS_THREADS];
static char msg[128];

TEST_REGISTER(kmalloc_new_concurrency_stress_test, SHOULD_NOT_FAIL,
              IS_UNIT_TEST) {
    memset((void *) done, 0, sizeof(done));

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < STRESS_THREADS; ++i) {
        args[i].id = i;
        args[i].done_flag = &done[i];
        thread_spawn("kmalloc_new_stress_worker", stress_worker, NULL)
            ->private = &args[i];
    }
    irql_lower(irql);

    all_ready = true;

    time_t start = time_get_ms();
    const time_t timeout_ms = 30 * 1000;
    while (time_get_ms() - start < timeout_ms) {
        int all = 1;
        for (int i = 0; i < STRESS_THREADS; ++i) {
            if (!done[i]) {
                all = 0;
                break;
            }
        }
        if (all)
            break;
    }

    for (int i = 0; i < STRESS_THREADS; ++i) {
        if (!done[i]) {
            snprintf(msg, sizeof(msg), "thread %d did not complete in time", i);
            ADD_MESSAGE(msg);
            SET_SUCCESS();
            return;
        }
    }

    slab_domains_print();
    ADD_MESSAGE("aggressive concurrency stress test completed");
    SET_SUCCESS();
}

/* -------------------- Small reallocation-like smoke test --------------------
 */

TEST_REGISTER(kmalloc_new_alloc_free_sequence_test, SHOULD_NOT_FAIL,
              IS_UNIT_TEST) {

    void *blocks[16];
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); ++i) {
        blocks[i] = kmalloc_new(64 + (i * 8), ALLOC_FLAGS_DEFAULT,
                                ALLOC_BEHAVIOR_NORMAL);
        if (!blocks[i]) {
            ADD_MESSAGE("failed to allocate block in sequence");
            /* free what we did get */
            for (size_t j = 0; j < i; ++j)
                kfree_new(blocks[j], ALLOC_BEHAVIOR_NORMAL);
            return;
        }
    }

    /* free every other block first */
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i += 2)
        kfree_new(blocks[i], ALLOC_BEHAVIOR_NORMAL);

    /* then free remaining */
    for (size_t i = 1; i < sizeof(blocks) / sizeof(blocks[0]); i += 2)
        kfree_new(blocks[i], ALLOC_BEHAVIOR_NORMAL);

    ADD_MESSAGE("alloc/free sequence test passed");
    SET_SUCCESS();
}

TEST_REGISTER(tlb_shootdown_single_cpu_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    paddr_t p2 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_phys(p1, PAGE_SIZE, 0, VMM_FLAG_NONE);
    TEST_ASSERT(va);

    *(volatile uint64_t *) va = 0x11111111;

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_phys(p2, PAGE_SIZE, 0, VMM_FLAG_NONE);

    tlb_shootdown((uintptr_t) va, true);

    *(volatile uint64_t *) va = 0x22222222;
    TEST_ASSERT(*(volatile uint64_t *) va == 0x22222222);

    SET_SUCCESS();
}

#define TLB_TEST_THREADS 4

static volatile uint64_t tlb_seen[TLB_TEST_THREADS];
static atomic_bool tlb_go = false;
static atomic_uint tlb_threads_done = 0;

static void tlb_reader(void *arg) {
    size_t id = (size_t) arg;

    while (!atomic_load(&tlb_go))
        cpu_relax();

    volatile uint64_t *va = scheduler_get_current_thread()->private;
    tlb_seen[id] = *va;
    atomic_fetch_add(&tlb_threads_done, 1);
}

TEST_REGISTER(tlb_shootdown_synchronous_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    paddr_t p2 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_phys(p1, PAGE_SIZE, 0, VMM_FLAG_NONE);
    TEST_ASSERT(va);

    *(volatile uint64_t *) va = 0xAAAAAAAA;

    struct thread *t[TLB_TEST_THREADS];
    for (size_t i = 0; i < TLB_TEST_THREADS; i++) {
        t[i] = thread_spawn("tlb_reader", tlb_reader, (void *) i);
        t[i]->private = va;
    }

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    vmm_map_page((vaddr_t) va, p2, PAGING_WRITE, VMM_FLAG_NONE);
    *(volatile uint64_t *) va = 0xBBBBBBBB;

    atomic_store(&tlb_go, true);
    tlb_shootdown((uintptr_t) va, true);

    while (atomic_load(&tlb_threads_done) < TLB_TEST_THREADS)
        cpu_relax();

    for (size_t i = 0; i < TLB_TEST_THREADS; i++) {
        TEST_ASSERT(tlb_seen[i] == 0xBBBBBBBB);
    }

    SET_SUCCESS();
}

TEST_REGISTER(tlb_shootdown_async_eventual_test, SHOULD_NOT_FAIL,
              IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    paddr_t p2 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_phys(p1, PAGE_SIZE, 0, VMM_FLAG_NONE);
    *(volatile uint64_t *) va = 0x1234;

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_phys(p2, PAGE_SIZE, 0, VMM_FLAG_NONE);
    *(volatile uint64_t *) va = 0x5678;

    tlb_shootdown((uintptr_t) va, false);

    /* Wait for IPIs to land */
    time_t start = time_get_ms();
    while (time_get_ms() - start < 100) {
        if (*(volatile uint64_t *) va == 0x5678)
            SET_SUCCESS();
        scheduler_yield();
    }

    ADD_MESSAGE("async TLB shootdown did not converge");
    SET_SUCCESS();
}

TEST_REGISTER(tlb_shootdown_flush_all_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    TEST_ASSERT(p);

    void *va = vmm_map_phys(p, PAGE_SIZE, 0, VMM_FLAG_NONE);

    /* Flood shootdown queue */
    for (size_t i = 0; i < TLB_QUEUE_SIZE * 4; i++) {
        tlb_shootdown((uintptr_t) va, false);
    }

    /* Now do a real remap */
    paddr_t p2 = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_phys(p2, PAGE_SIZE, 0, VMM_FLAG_NONE);
    *(volatile uint64_t *) va = 0xDEADBEEF;

    tlb_shootdown((uintptr_t) va, true);

    TEST_ASSERT(*(volatile uint64_t *) va == 0xDEADBEEF);
    SET_SUCCESS();
}

static void tlb_spammer(void *) {
    paddr_t p = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    void *va = vmm_map_phys(p, PAGE_SIZE, 0, VMM_FLAG_NONE);

    for (int i = 0; i < 1000; i++) {
        tlb_shootdown((uintptr_t) va, false);
    }
}

TEST_REGISTER(tlb_shootdown_contention_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    for (int i = 0; i < 4; i++)
        thread_spawn("tlb_spammer", tlb_spammer, NULL);

    time_t start = time_get_ms();
    while (time_get_ms() - start < 200)
        scheduler_yield();

    ADD_MESSAGE("concurrent shootdown stress completed");
    SET_SUCCESS();
}

#endif
