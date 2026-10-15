#include <console/printf.h>
#include <math/units.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/hhdm.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>

uint8_t boot_bitmap[BOOT_BITMAP_SIZE] = {[0 ...(BOOT_BITMAP_SIZE - 1)] = 0xFF};
uint64_t bitmap_size = BOOT_BITMAP_SIZE;

uint8_t *bitmap;
static uint64_t last_allocated_index = 0;

paddr_t bitmap_alloc_page() {
    return bitmap_alloc_pages(1, 0);
}

paddr_t bitmap_alloc_pages(uint64_t count, enum alloc_flags flags) {
    cc_ignore(flags);
    if (count == 0)
        panic("Zero pages requested");

    uint64_t consecutive = 0;
    uint64_t start_index = 0;
    bool found = false;

    for (uint64_t i = last_allocated_index; i < to_bits(bitmap_size); i++) {
        if (!test_bit(i)) {
            if (consecutive == 0)
                start_index = i;

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
        for (uint64_t i = 0; i < to_bits(bitmap_size); i++) {
            if (!test_bit(i)) {
                if (consecutive == 0)
                    start_index = i;

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
        return 0x0;

    last_allocated_index = start_index;
    for (uint64_t i = 0; i < count; i++) {
        set_bit(start_index + i);
    }

    return (paddr_t) (start_index * PAGE_SIZE);
}

void bitmap_free_pages(paddr_t addr, uint64_t count) {
    if (addr == 0 || count == 0) {
        panic("Possible UAF");
    }

    uint64_t start_index = (uint64_t) addr / PAGE_SIZE;

    if (start_index >= to_bits(bitmap_size) ||
        start_index + count > to_bits(bitmap_size)) {
        printf("Invalid address range to free: 0x%zx with count %zu\n",
               (uint64_t) addr, count);
        return;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint64_t index = start_index + i;
        if (test_bit(index)) {
            clear_bit(index);
        } else {
            printf("Page at 0x%zx was already free\n",
                   hhdm_paddr_to_vaddr(index * PAGE_SIZE));
        }
    }
}
