#pragma once
#include <charmos.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/types.h>

struct free_area;
void buddy_add_to_free_area(struct page *page, struct free_area *area);
struct page *buddy_remove_from_free_area(struct free_area *area);
paddr_t buddy_alloc_pages_global(size_t count, enum alloc_flags f);
void buddy_free_pages_global(paddr_t addr, uint64_t count);
struct limine_memmap_entry;
void buddy_add_entry(struct page *page_array, struct limine_memmap_entry *entry,
                     struct free_area *farea);

paddr_t buddy_alloc_pages(struct free_area *free_area, size_t count);
void buddy_free_pages(paddr_t addr, size_t count, struct free_area *free_area,
                      size_t total_pages);
void buddy_init(void);
