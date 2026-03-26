/* @title: Physical memory manager */
#pragma once
#include <compiler.h>
#include <limine.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <types/types.h>

extern struct limine_memmap_response *memmap;
paddr_t pmm_alloc_page_internal(enum alloc_flags flags);
paddr_t pmm_alloc_pages_internal(size_t count, enum alloc_flags flags);

void pmm_free_pages(paddr_t addr, uint64_t count);
void pmm_free_page(paddr_t addr);

void pmm_early_init(struct limine_memmap_request m);
void pmm_mid_init(void);
void pmm_late_init(void);

uint64_t pmm_get_usable_ram(void);

/* The reason we use an inline function here is to allow a check of
 * `count` to make sure it somehow has not ended up equaling
 * ALLOC_PARAMS_DEFAULT and warning once if this has ended up being the case */
#define pmm_alloc_pages_1(count)                                               \
    ({                                                                         \
        if ((enum alloc_flags) count == ALLOC_FLAGS_DEFAULT)                   \
            log_warn_once("Input to alloc_pages matches ALLOC_FLAGS_DEFAULT, " \
                          "possible mistake");                                 \
                                                                               \
        pmm_alloc_pages_internal(count, ALLOC_FLAGS_DEFAULT);                  \
    })

#define pmm_alloc_pages_2(count, f) pmm_alloc_pages_internal((count), (f));

#define pmm_alloc_pages(...)                                                   \
    _DISPATCH(pmm_alloc_pages, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define pmm_alloc_page_0() pmm_alloc_page_internal((ALLOC_FLAGS_DEFAULT))
#define pmm_alloc_page_1(f) pmm_alloc_page_internal((f))

#define pmm_alloc_page(...)                                                    \
    _DISPATCH(pmm_alloc_page, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
