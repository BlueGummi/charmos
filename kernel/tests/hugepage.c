#include <asm.h>
#include <mem/hugepage.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <tests.h>

#define HUGEPAGE_SINGLE_PAGE_ALLOC_TEST_TIMES 400
static uint64_t clock_cycles[HUGEPAGE_SINGLE_PAGE_ALLOC_TEST_TIMES] = {0};

REGISTER_TEST(hugepage_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    /* Initial allocation */
    void *ptr = hugepage_alloc_pages(1);
    TEST_ASSERT(ptr);

    for (uint64_t i = 0; i < HUGEPAGE_SINGLE_PAGE_ALLOC_TEST_TIMES; i++) {
        uint64_t start = rdtsc();
        hugepage_alloc_pages(1);
        uint64_t end = rdtsc();
        clock_cycles[i] = end - start;
    }

    uint64_t total = 0;
    for (uint64_t i = 0; i < HUGEPAGE_SINGLE_PAGE_ALLOC_TEST_TIMES; i++)
        total += clock_cycles[i];

    char *message = kzalloc(100);
    snprintf(message, 100,
             "The average amount of clock cycles for a single page hugepage "
             "alloc is %d",
             total / HUGEPAGE_SINGLE_PAGE_ALLOC_TEST_TIMES);
    ADD_MESSAGE(message);

    ptr = hugepage_alloc_pages(1024);

    /*    for (size_t i = 0; i < 512; i++)
            hugepage_alloc_pages(512); */

    TEST_ASSERT(ptr);

    hugepage_free_pages(ptr, 1024);

    ptr = hugepage_alloc_pages(512);
    uint64_t tsc = rdtsc();
    hugepage_lookup(ptr);
    uint64_t firsttsc = rdtsc() - tsc;
    tsc = rdtsc();
    hugepage_lookup(ptr);
    uint64_t secondtsc = rdtsc() - tsc;

    char *msg2 = kzalloc(100);
    snprintf(msg2, 100,
             "The first hugepage lookup took %d cc, the second one took %d cc",
             firsttsc, secondtsc);
    ADD_MESSAGE(msg2);

    SET_SUCCESS;
}
