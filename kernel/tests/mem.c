#include <crypto/prng.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>

REGISTER_TEST(pmm_alloc_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page(ALLOC_FLAGS_NONE);
    TEST_ASSERT(p);
    SET_SUCCESS();
}

REGISTER_TEST(vmm_map_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    uint64_t p = pmm_alloc_page(ALLOC_FLAGS_NONE);
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_phys(p, PAGE_SIZE, 0);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, PAGE_SIZE);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr) == (uint64_t) -1);
    SET_SUCCESS();
}

/* probably don't need these at all but I'll keep
 * them in case something decides to be funny */
#define ALIGNED_ALLOC_TIMES 512

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    REGISTER_TEST(kmalloc_aligned_##name##_test, false, false) {               \
        ABORT_IF_RAM_LOW();                                                    \
        for (uint64_t i = 0; i < ALIGNED_ALLOC_TIMES; i++) {                   \
            void *ptr = kmalloc_aligned(align, align);                         \
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
REGISTER_TEST(pmm_stress_alloc_free_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        pmm_stress_test_ptrs[i] = pmm_alloc_page(ALLOC_FLAGS_NONE);
        TEST_ASSERT(pmm_stress_test_ptrs[i] != 0);
    }

    for (int64_t i = STRESS_ALLOC_TIMES - 1; i >= 0; i--) {
        pmm_free_page(pmm_stress_test_ptrs[i]);
    }

    SET_SUCCESS();
}

static void *stress_alloc_free_ptrs[STRESS_ALLOC_TIMES] = {0};
REGISTER_TEST(kmalloc_stress_alloc_free_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        stress_alloc_free_ptrs[i] = kmalloc(64);
        TEST_ASSERT(stress_alloc_free_ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % STRESS_ALLOC_TIMES;
        if (stress_alloc_free_ptrs[idx]) {
            kfree(stress_alloc_free_ptrs[idx]);
            stress_alloc_free_ptrs[idx] = NULL;
        }
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        if (stress_alloc_free_ptrs[i]) {
            kfree(stress_alloc_free_ptrs[i]);
        }
    }

    SET_SUCCESS();
}

/* Put it here to avoid it eating things up */
static void *mixed_stress_test_ptrs[STRESS_ALLOC_TIMES] = {0};
REGISTER_TEST(kmalloc_mixed_stress_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        mixed_stress_test_ptrs[i] = kmalloc(128);
        TEST_ASSERT(mixed_stress_test_ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        kfree(mixed_stress_test_ptrs[i]);
    }

    SET_SUCCESS();
}

#define MT_THREAD_COUNT 8
#define MT_ALLOC_TIMES 1024

static void mt_kmalloc_worker() {
    void *ptrs[MT_ALLOC_TIMES] = {0};

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        ptrs[i] = kmalloc(64);
        TEST_ASSERT(ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % MT_ALLOC_TIMES;

        if (ptrs[idx]) {
            kfree(ptrs[idx]);
            ptrs[idx] = NULL;
        }
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }
}

REGISTER_TEST(kmalloc_multithreaded_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    struct thread *threads[MT_THREAD_COUNT];

    for (int i = 0; i < MT_THREAD_COUNT; i++) {
        threads[i] =
            thread_spawn_custom_stack(mt_kmalloc_worker, PAGE_SIZE * 16);
        TEST_ASSERT(threads[i] != NULL);
    }

    SET_SUCCESS();
}

#define MT_PMM_THREAD_COUNT 8
#define MT_PMM_ALLOC_TIMES 512

static void mt_pmm_worker() {
    paddr_t addrs[MT_PMM_ALLOC_TIMES];

    for (uint64_t i = 0; i < MT_PMM_ALLOC_TIMES; i++) {
        addrs[i] = pmm_alloc_page(ALLOC_FLAGS_NONE);
        TEST_ASSERT(addrs[i] != 0);
    }

    for (int64_t i = MT_PMM_ALLOC_TIMES - 1; i >= 0; i--) {
        pmm_free_page(addrs[i]);
    }
}

REGISTER_TEST(pmm_multithreaded_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    struct thread *threads[MT_PMM_THREAD_COUNT];

    for (int i = 0; i < MT_PMM_THREAD_COUNT; i++) {
        threads[i] = thread_spawn_custom_stack(mt_pmm_worker, PAGE_SIZE * 16);
        TEST_ASSERT(threads[i] != NULL);
    }

    SET_SUCCESS();
}
