#include <limine.h>
#include <stdbool.h>
#include <stddef.h>

void init_physical_allocator(uint64_t o, struct limine_memmap_request m);
void *pmm_alloc_page(bool offset);
void *pmm_alloc_pages(size_t count, bool add_offset);
void pmm_free_pages(void *addr, size_t count, bool has_offset);
void print_memory_status();
#pragma once
