#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void vmm_bitmap_init(uintptr_t base_address, size_t total_pages);
void *vmm_alloc_pages(size_t count);
void *vmm_alloc_page();
void vmm_free_pages(void *address, size_t count);
size_t vmm_get_free_pages(void);
size_t vmm_get_total_pages(void);
void vmalloc_set_offset(uint64_t o);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *addr, size_t size);
void set_map_location();
void unset_map_location();
void vmm_print_memory_status();
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
