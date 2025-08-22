#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <types/types.h>

extern struct limine_memmap_response *memmap;
void pmm_early_init(struct limine_memmap_request m);
paddr_t pmm_alloc_page();
paddr_t pmm_alloc_pages(uint64_t count);
void pmm_free_pages(paddr_t addr, uint64_t count);
void pmm_mid_init(void);
uint64_t pmm_get_usable_ram(void);
#pragma once
