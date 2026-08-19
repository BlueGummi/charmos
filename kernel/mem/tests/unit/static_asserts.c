#include <math/align.h>
#include <math/div.h>
#include <mem/page.h>
#include <test/static_assert.h>

static_assert_size(struct page, 8);
static_assert_size(struct page_table, 4096);
static_assert(PAGE_SIZE == 4096, "PAGE_SIZE must be 4096");
static_assert(PAGE_4K_SHIFT == 12, "PAGE_4K_SHIFT must be 12");

static_assert(DIV_ROUND_UP(0, 4) == 0, "DIV_ROUND_UP(0, 4) == 0");
static_assert(DIV_ROUND_UP(1, 4) == 1, "DIV_ROUND_UP(1, 4) == 1");
static_assert(DIV_ROUND_UP(4, 4) == 1, "DIV_ROUND_UP(4, 4) == 1");
static_assert(DIV_ROUND_UP(5, 4) == 2, "DIV_ROUND_UP(5, 4) == 2");

static_assert(ALIGN_UP(4095, 4096) == 4096, "ALIGN_UP(4095, 4096) == 4096");
static_assert(ALIGN_DOWN(4097, 4096) == 4096, "ALIGN_DOWN(4097, 4096) == 4096");
