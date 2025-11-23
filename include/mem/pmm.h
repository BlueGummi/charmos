/* @title: Physical memory manager */
#include <limine.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <types/types.h>

extern struct limine_memmap_response *memmap;
paddr_t pmm_alloc_page(enum alloc_flags flags);
paddr_t pmm_alloc_pages(uint64_t count, enum alloc_flags flags);

void pmm_free_pages(paddr_t addr, uint64_t count);
void pmm_free_page(paddr_t addr);

void pmm_early_init(struct limine_memmap_request m);
void pmm_mid_init(void);
void pmm_late_init(void);

uint64_t pmm_get_usable_ram(void);
#pragma once
