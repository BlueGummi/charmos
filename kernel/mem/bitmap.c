#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>

#define BOOT_BITMAP_SIZE ((1024 * 1024 * 128) / PAGE_SIZE / 8)

uint64_t bitmap_size = BOOT_BITMAP_SIZE;

uint8_t *bitmap;
static uint64_t last_allocated_index = 0;

void *bitmap_alloc_page(bool add_offset) {
    return pmm_alloc_pages(1, add_offset);
}

void *bitmap_alloc_pages(uint64_t count, bool add_offset) {
    if (count == 0) {
        k_panic("Zero pages requested\n");
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

    /* fail */
    if (!found)
        return NULL;

    last_allocated_index = start_index;
    for (uint64_t i = 0; i < count; i++) {
        set_bit(start_index + i);
    }

    return (void *) ((add_offset ? global.hhdm_offset : 0) +
                     (start_index * PAGE_SIZE));
}

void bitmap_free_pages(void *addr, uint64_t count, bool has_offset) {
    if (addr == NULL || count == 0) {
        k_panic("Possible UAF\n");
    }

    uint64_t start_index =
        ((uint64_t) addr - (has_offset ? global.hhdm_offset : 0)) / PAGE_SIZE;

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
                     global.hhdm_offset + (index * PAGE_SIZE));
        }
    }
}
