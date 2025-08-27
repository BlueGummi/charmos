#include <crypto/prng.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>

REGISTER_TEST(pmm_alloc_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);
    TEST_ASSERT(p);
    SET_SUCCESS;
}

REGISTER_TEST(vmm_map_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    uint64_t p = pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_phys(p, PAGE_SIZE, 0);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, PAGE_SIZE);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr) == (uint64_t) -1);
    SET_SUCCESS;
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
        SET_SUCCESS;                                                           \
    }

KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)

REGISTER_TEST(kmalloc_aligned_4096_test, false, false) {
    ABORT_IF_RAM_LOW();
    for (uint64_t i = 0; i < ALIGNED_ALLOC_TIMES; i++) {
        void *ptr = hugepage_alloc_page();
        TEST_ASSERT(ptr != NULL);
        ASSERT_ALIGNED(ptr, 4096);
    }
    SET_SUCCESS;
}

#define STRESS_ALLOC_TIMES 2048

REGISTER_TEST(pmm_stress_alloc_free_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    paddr_t addrs[STRESS_ALLOC_TIMES];

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        addrs[i] = pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);
        TEST_ASSERT(addrs[i] != 0);
    }

    for (int64_t i = STRESS_ALLOC_TIMES - 1; i >= 0; i--) {
        pmm_free_page(addrs[i]);
    }

    SET_SUCCESS;
}

REGISTER_TEST(kmalloc_stress_alloc_free_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    void *ptrs[STRESS_ALLOC_TIMES];

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        ptrs[i] = kmalloc(64);
        TEST_ASSERT(ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % STRESS_ALLOC_TIMES;
        if (ptrs[idx]) {
            kfree(ptrs[idx]);
            ptrs[idx] = NULL;
        }
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
        }
    }

    SET_SUCCESS;
}

REGISTER_TEST(kmalloc_mixed_stress_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    void *small_ptrs[STRESS_ALLOC_TIMES];

    void *huge_ptrs[STRESS_ALLOC_TIMES / 8];

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        small_ptrs[i] = kmalloc(128);
        TEST_ASSERT(small_ptrs[i] != NULL);
        if (i % 8 == 0) {
            huge_ptrs[i / 8] = hugepage_alloc_page();
            TEST_ASSERT(huge_ptrs[i / 8] != NULL);
        }
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES / 8; i++) {
        hugepage_free_page(huge_ptrs[i]);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        kfree(small_ptrs[i]);
    }

    SET_SUCCESS;
}

REGISTER_TEST(kmalloc_alignment_stress_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    const size_t alignments[] = {16, 32, 64, 128, 256, 512, 1024};
    void *ptrs[STRESS_ALLOC_TIMES];

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        size_t align =
            alignments[i % (sizeof(alignments) / sizeof(alignments[0]))];
        ptrs[i] = kmalloc_aligned(align, align);
        TEST_ASSERT(ptrs[i] != NULL);
        ASSERT_ALIGNED(ptrs[i], align);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        kfree_aligned(ptrs[i]);
    }

    SET_SUCCESS;
}

#define MT_THREAD_COUNT 8
#define MT_ALLOC_TIMES 512

static void mt_kmalloc_worker() {
    void *ptrs[MT_ALLOC_TIMES];

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
        threads[i] = thread_spawn(mt_kmalloc_worker);
        TEST_ASSERT(threads[i] != NULL);
    }

    SET_SUCCESS;
}

static void mt_hugepage_worker() {
    void *ptrs[MT_ALLOC_TIMES];

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        ptrs[i] = hugepage_alloc_page();

        TEST_ASSERT(ptrs[i] != NULL);
        ASSERT_ALIGNED(ptrs[i], 4096);
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        hugepage_free_page(ptrs[i]);
    }
}

REGISTER_TEST(hugepage_multithreaded_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ABORT_IF_RAM_LOW();

    struct thread *threads[MT_THREAD_COUNT];

    for (int i = 0; i < MT_THREAD_COUNT; i++) {
        threads[i] = thread_spawn(mt_hugepage_worker);
        TEST_ASSERT(threads[i] != NULL);
    }

    SET_SUCCESS;
}

#define MT_PMM_THREAD_COUNT 8
#define MT_PMM_ALLOC_TIMES 512

static void mt_pmm_worker() {
    paddr_t addrs[MT_PMM_ALLOC_TIMES];

    for (uint64_t i = 0; i < MT_PMM_ALLOC_TIMES; i++) {
        addrs[i] = pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);
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
        threads[i] = thread_spawn(mt_pmm_worker);
        TEST_ASSERT(threads[i] != NULL);
    }

    SET_SUCCESS;
}
