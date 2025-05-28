#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void bitmap_bitmap_init(uintptr_t base_address, size_t total_pages);
void *bitmap_alloc_pages(size_t count);
void bitmap_free_pages(void *address, size_t count);
void set_map_location();
void unset_map_location();
void bitmap_print_memory_status();
#define BITMAP_SIZE (1 << 20) // four giggybites
#define BITS_PER_ENTRY (sizeof(uint64_t) * 8)

/*
 * The VMM bitmap allocator, containing page counts, free page counts, addresses
 * for the first index, and the bitmap address
 */
struct vmalloc_bitmap {
    uint64_t *bitmap;
    uintptr_t base_address;
    size_t total_pages;
    size_t free_pages;
};

#pragma once
