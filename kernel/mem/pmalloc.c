#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "limine.h"

#define PAGE_SIZE 4096

#define BOOT_BITMAP_SIZE ((1024 * 1024 * 128) / PAGE_SIZE / 8)

static uint8_t boot_bitmap[BOOT_BITMAP_SIZE];
static bool buddy_active = false;

static struct limine_memmap_response *memmap;
static uint64_t total_pages = 0;

void pmm_init(struct limine_memmap_request m) {
    bitmap = boot_bitmap;
    memset(bitmap, 0xFF, BOOT_BITMAP_SIZE);

    memmap = m.response;

    if (memmap == NULL || memmap->entries == NULL) {
        k_panic("Failed to retrieve Limine memory map\n");
        return;
    }

    uint64_t total_phys = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_phys += entry->length;
            uint64_t start = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t end = (entry->base + entry->length) & ~(PAGE_SIZE - 1);

            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                uint64_t index = addr / PAGE_SIZE;
                if (index < BOOT_BITMAP_SIZE * 8) {
                    clear_bit(index);
                }
            }
        }
    }
    total_pages = total_phys / PAGE_SIZE;
}

uint64_t pmm_get_usable_ram(void) {
    return total_pages * PAGE_SIZE;
}

void pmm_dyn_init() {
    uint64_t size = total_pages / 8;
    uint8_t *new_bitmap = kmalloc(size);
    if (!new_bitmap)
        k_panic("Could not allocate space for physical memory allocator\n");

    uint64_t to_copy = BOOT_BITMAP_SIZE > size ? size : BOOT_BITMAP_SIZE;
    memcpy(new_bitmap, bitmap, to_copy);
    bitmap_size = total_pages / 8;
    bitmap = new_bitmap;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t end = (entry->base + entry->length) & ~(PAGE_SIZE - 1);

            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                uint64_t index = addr / PAGE_SIZE;
                if (index >= BOOT_BITMAP_SIZE * 8 && index < total_pages)
                    clear_bit(index);
            }
        }
    }
}

void *pmm_alloc_page(bool add_offset) {
    if (!buddy_active)
        return bitmap_alloc_page(add_offset);

    k_panic("No buddy allocator yet\n");
}

void *pmm_alloc_pages(uint64_t count, bool add_offset) {
    if (!buddy_active)
        return bitmap_alloc_pages(count, add_offset);

    k_panic("No buddy allocator yet\n");
}

void pmm_free_pages(void *addr, uint64_t count, bool has_offset) {
    if (!buddy_active)
        return bitmap_free_pages(addr, count, has_offset);

    k_panic("No buddy allocator yet\n");
}
