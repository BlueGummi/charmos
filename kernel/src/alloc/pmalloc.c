#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <memfuncs.h>
#include <printf.h>

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
 * Free a non-offsetted page from the PMM bitmap
 */
void free_page(void *addr) {
    size_t index = (size_t) addr / PAGE_SIZE;
    if (index >= BITMAP_SIZE * 8) {
        k_printf("Invalid address to free: 0x%zx\n", (size_t) addr);
        return;
    }
    clear_bit(index);
    k_printf("Freed page at 0x%zx\n", (size_t) addr);
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

    k_printf("\nDetailed Allocation Map:\n");
    for (size_t i = 0; i < total_pages; i++) {
        if (i % 64 == 0) {
            k_printf("\n");
        }
        k_printf("%c", test_bit(i) ? 'X' : '.');
    }
    k_printf("\n");
}

/*
 * Allocate `count` pages.
 */
void *pmm_alloc_pages(size_t count) {
    if (count == 0) {
        return NULL;
    }

    if (count == 1) {
        return pmm_alloc_page(false);
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

    return (void *) (offset + (start_index * PAGE_SIZE));
}

/*
 * Free `count` pages, starting at `addr`.
 *
 * Addresses should have the HHDM offset added to them.
 */
void pmm_free_pages(void *addr, size_t count) {
    if (addr == NULL || count == 0) {
        return;
    }

    size_t start_index = ((size_t) addr - offset) / PAGE_SIZE;

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
