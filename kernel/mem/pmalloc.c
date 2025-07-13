#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "limine.h"

#define PAGE_SIZE 4096

#define BOOT_BITMAP_SIZE ((1024 * 1024 * 128) / PAGE_SIZE / 8)

static uint64_t bitmap_size = BOOT_BITMAP_SIZE;

static uint8_t boot_bitmap[BOOT_BITMAP_SIZE];
static uint8_t *bitmap;
static uint64_t last_allocated_index = 0;

static void set_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t mask = 1 << (index % 8);
    __atomic_fetch_or(&bitmap[byte], mask, __ATOMIC_SEQ_CST);
}

static void clear_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t mask = ~(1 << (index % 8));
    __atomic_fetch_and(&bitmap[byte], mask, __ATOMIC_SEQ_CST);
}

static bool test_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t value;
    __atomic_load(&bitmap[byte], &value, __ATOMIC_SEQ_CST);
    return (value & (1 << (index % 8))) != 0;
}

static uint64_t offset = 0;
static struct limine_memmap_response *memmap;
static uint64_t total_pages = 0;

void pmm_init(uint64_t o, struct limine_memmap_request m) {

    offset = o;
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

void pmm_dyn_init() {
    uint8_t *new_bitmap = kmalloc(total_pages / 8);
    memcpy(new_bitmap, bitmap, BOOT_BITMAP_SIZE);
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
    return pmm_alloc_pages(1, add_offset);
}

void *pmm_alloc_pages(uint64_t count, bool add_offset) {

    if (count == 0) {
        return NULL;
    }

    uint64_t consecutive = 0;
    uint64_t start_index = 0;
    bool found = false;

    for (uint64_t i = last_allocated_index; i < bitmap_size * 8; i++) {
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
        for (uint64_t i = 0; i < bitmap_size * 8; i++) {
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
    }

    if (!found) {
        k_printf("Couldn't allocate %zu contiguous pages\n", count);
        return NULL;
    }

    last_allocated_index = start_index;
    for (uint64_t i = 0; i < count; i++) {
        set_bit(start_index + i);
    }

    return (void *) ((add_offset ? offset : 0) + (start_index * PAGE_SIZE));
}

void pmm_free_pages(void *addr, uint64_t count, bool has_offset) {

    if (addr == NULL || count == 0) {
        return;
    }

    uint64_t start_index =
        ((uint64_t) addr - (has_offset ? offset : 0)) / PAGE_SIZE;

    if (start_index >= bitmap_size * 8 ||
        start_index + count > bitmap_size * 8) {
        k_printf("Invalid address range to free: 0x%zx with count %zu\n",
                 (uint64_t) addr, count);
        return;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint64_t index = start_index + i;
        if (test_bit(index)) {
            clear_bit(index);
        } else {
            k_printf("Page at 0x%zx was already free\n",
                     offset + (index * PAGE_SIZE));
        }
    }
}
