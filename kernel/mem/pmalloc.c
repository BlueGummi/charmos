#include "limine.h"
#include <console/printf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PAGE_SIZE 4096

#define BITMAP_SIZE (0x100000000 / PAGE_SIZE / 8)

static uint8_t bitmap[BITMAP_SIZE];

static void set_bit(size_t index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static void clear_bit(size_t index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static bool test_bit(size_t index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

uint64_t offset = 0;
void init_physical_allocator(uint64_t o, struct limine_memmap_request m) {

    offset = o;
    memset(bitmap, 0xFF, BITMAP_SIZE);

    struct limine_memmap_response *memdata = m.response;

    if (memdata == NULL || memdata->entries == NULL) {
        k_printf("Failed to retrieve Limine memory map\n");
        return;
    }

    for (uint64_t i = 0; i < memdata->entry_count; i++) {
        struct limine_memmap_entry *entry = memdata->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {

            uint64_t start = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t end = (entry->base + entry->length) & ~(PAGE_SIZE - 1);

            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                size_t index = addr / PAGE_SIZE;
                if (index < BITMAP_SIZE * 8) {
                    clear_bit(index);
                }
            }
        }
    }
}

void *pmm_alloc_page(bool add_offset) {

    for (size_t i = 0; i < BITMAP_SIZE * 8; i++) {
        if (!test_bit(i)) {
            set_bit(i);
            void *page = (void *) ((add_offset ? offset : 0) + (i * PAGE_SIZE));
            return page;
        }
    }

    return NULL;
}

/*
 * Give an overview of the PMM's bitmap state.
 * Used for debug and log.
 */
void print_memory_status() {
    size_t total_pages = BITMAP_SIZE * 8;
    size_t free_pages = 0;
    size_t allocated_pages = 0;

    for (size_t i = 0; i < total_pages; i++) {
        if (!test_bit(i)) {
            free_pages++;
        } else {
            allocated_pages++;
        }
    }

    k_printf("Memory Status:\n");
    k_printf("  Total Pages: %zu\n", total_pages);
    k_printf("  Free Pages: %zu\n", free_pages);
    k_printf("  Allocated Pages: %zu\n", allocated_pages);
    k_printf("  Memory Usage: %d%%\n", (allocated_pages * 100) / total_pages);

    k_printf("\nMemory Segments (contiguous):\n");

    size_t segment_start = 0;
    int segment_state = test_bit(0);

    for (size_t i = 1; i <= total_pages; i++) {
        int current_state = (i < total_pages) ? test_bit(i) : -1;
        if (current_state != segment_state) {

            uintptr_t start_addr = segment_start * PAGE_SIZE;
            uintptr_t end_addr = (i - 1) * PAGE_SIZE + PAGE_SIZE - 1;

            k_printf("  %c: 0x%016lx - 0x%016lx (%zu pages)\n",
                     segment_state ? 'A' : 'F', (unsigned long) start_addr,
                     (unsigned long) end_addr, i - segment_start);

            segment_start = i;
            segment_state = current_state;
        }
    }

    k_printf("\n");
}

/*
 * Allocate `count` pages.
 */
void *pmm_alloc_pages(size_t count, bool add_offset) {

    if (count == 0) {
        return NULL;
    }

    if (count == 1) {
        return pmm_alloc_page(add_offset);
    }

    size_t consecutive = 0;
    size_t start_index = 0;
    bool found = false;

    for (size_t i = 0; i < BITMAP_SIZE * 8; i++) {

        if (!test_bit(i)) {
            if (consecutive == 0) {
                start_index = i;
            }
            consecutive++;

            if (consecutive == count) {
                found = true;
                break;
            }
        } else {
            consecutive = 0;
        }
    }

    if (!found) {
        k_printf("Couldn't allocate %zu contiguous pages\n", count);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        set_bit(start_index + i);
    }

    return (void *) ((add_offset ? offset : 0) + (start_index * PAGE_SIZE));
}

/*
 * Free `count` pages, starting at `addr`.
 *
 * Addresses should have the HHDM offset added to them.
 */
void pmm_free_pages(void *addr, size_t count, bool has_offset) {

    if (addr == NULL || count == 0) {
        return;
    }

    size_t start_index =
        ((size_t) addr - (has_offset ? offset : 0)) / PAGE_SIZE;

    if (start_index >= BITMAP_SIZE * 8 ||
        start_index + count > BITMAP_SIZE * 8) {
        k_printf("Invalid address range to free: 0x%zx with count %zu\n",
                 (size_t) addr, count);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        size_t index = start_index + i;
        if (test_bit(index)) {
            clear_bit(index);
        } else {
            k_printf("Page at 0x%zx was already free\n",
                     offset + (index * PAGE_SIZE));
        }
    }
}
