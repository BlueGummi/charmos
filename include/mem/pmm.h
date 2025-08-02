#include <limine.h>
#include <stdbool.h>
#include <stddef.h>

void pmm_init(struct limine_memmap_request m);
void *pmm_alloc_page(bool offset);
void *pmm_alloc_pages(uint64_t count, bool add_offset);
void pmm_free_pages(void *addr, uint64_t count, bool has_offset);
void pmm_dyn_init(void);
uint64_t pmm_get_usable_ram(void);
#pragma once
