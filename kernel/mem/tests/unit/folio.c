#include "../test_internal.h"

#ifdef TEST_FOLIO
TEST_GROUP_DECLARE(folio, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

TEST_DECLARE_UNIT(folio_backpointers, .group = TEST_GROUP(folio)) {
    for (uint8_t order = 0; order <= 3; order++) {
        struct folio *f = folio_alloc(order);
        TEST_ASSERT(f);
        TEST_ASSERT(f->order == order);
        TEST_ASSERT(folio_nr_pages(f) == (1ul << order));
        TEST_ASSERT(!folio_mapped(f));
        TEST_ASSERT(atomic_load(&f->mapcount) == 0);

        for (size_t n = 0; n < folio_nr_pages(f); n++) {
            struct page *p = folio_get_page(f, n);
            TEST_ASSERT(folio_from_paddr(folio_get_paddr_for(f, n)) == f);
            TEST_ASSERT(folio_from_page(p) == f);
            TEST_ASSERT(page_get_folio_index(p) == n);
            TEST_ASSERT(page_is_folio_head(p) == (n == 0));
        }
    }
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(folio_zero_copy, .group = TEST_GROUP(folio),
                  TEST_INTENSITY(16, 128, 1024)) {
    size_t iters = ctx->intensity_val ? ctx->intensity_val : 128;
    for (size_t iter = 0; iter < iters; iter++) {
        struct folio *src = folio_alloc(1); /* 2 pages */
        struct folio *dst = folio_alloc(1);
        TEST_ASSERT(src && dst);

        for (size_t n = 0; n < folio_nr_pages(src); n++) {
            uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
            uint8_t *d = (uint8_t *) folio_get_vaddr_for(dst, n);
            for (size_t b = 0; b < PAGE_SIZE; b++) {
                s[b] = (uint8_t) (b + n * 7 + 1);
                d[b] = 0xAB;
            }
        }

        folio_zero(src);
        for (size_t n = 0; n < folio_nr_pages(src); n++) {
            uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
            for (size_t b = 0; b < PAGE_SIZE; b++)
                TEST_ASSERT(s[b] == 0);
        }

        for (size_t n = 0; n < folio_nr_pages(src); n++) {
            uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
            for (size_t b = 0; b < PAGE_SIZE; b++)
                s[b] = (uint8_t) (b * 3 + n + 5);
        }
        folio_copy(src, dst);
        for (size_t n = 0; n < folio_nr_pages(dst); n++) {
            uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
            uint8_t *d = (uint8_t *) folio_get_vaddr_for(dst, n);
            TEST_ASSERT(memcmp(s, d, PAGE_SIZE) == 0);
        }
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(folio_anon_tag_mapcount, .group = TEST_GROUP(folio)) {
    struct folio *f = folio_alloc(0);
    TEST_ASSERT(f);

    TEST_ASSERT(!folio_is_anon(f));

    struct anon_vma *av = anon_vma_alloc();
    TEST_ASSERT(av);

    folio_set_anon(f, av, 0xDEAD);
    TEST_ASSERT(folio_is_anon(f));
    TEST_ASSERT(folio_get_anon_vma(f) == av);
    TEST_ASSERT(((uintptr_t) f->mapping & ~FOLIO_TAG_BITS) == (uintptr_t) av);
    TEST_ASSERT(f->index == 0xDEAD);

    TEST_ASSERT(!folio_mapped(f));
    folio_mapcount_inc(f);
    TEST_ASSERT(folio_mapped(f) && atomic_load(&f->mapcount) == 1);
    folio_mapcount_inc(f);
    TEST_ASSERT(atomic_load(&f->mapcount) == 2);
    TEST_ASSERT(folio_mapcount_dec(f) == false);
    TEST_ASSERT(folio_mapcount_dec(f) == true);
    TEST_ASSERT(!folio_mapped(f));

    return TEST_SUCCESS;
}
#endif
