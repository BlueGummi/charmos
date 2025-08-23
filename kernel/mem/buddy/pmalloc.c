#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <misc/align.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct limine_memmap_response *memmap;

void pmm_early_init(struct limine_memmap_request m) {
    bitmap = boot_bitmap;
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
            uint64_t start = ALIGN_DOWN(entry->base, PAGE_SIZE);
            uint64_t end = ALIGN_UP(entry->base + entry->length, PAGE_SIZE);

            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                uint64_t index = addr / PAGE_SIZE;
                if (index < BOOT_BITMAP_SIZE * 8) {
                    clear_bit(index);
                }
            }
        }
    }
    global.total_pages = total_phys / PAGE_SIZE;
}

void pmm_mid_init() {
    size_t pages_needed =
        (sizeof(struct buddy_page) * global.total_pages + PAGE_SIZE - 1) /
        PAGE_SIZE;

    bool found = false;

    for (uint64_t i = 0; i < memmap->entry_count && !found; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);

        uint64_t run_start = 0;

        uint64_t run_len = 0;

        for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
            uint64_t pfn = addr / PAGE_SIZE;

            if (!test_bit(pfn)) {
                if (run_len == 0)
                    run_start = addr;
                run_len++;

                if (run_len >= pages_needed) {
                    buddy_page_array =
                        (void *) (run_start + global.hhdm_offset);

                    for (uint64_t j = 0; j < pages_needed; j++)
                        set_bit((run_start / PAGE_SIZE) + j);

                    entry->base = run_start + pages_needed * PAGE_SIZE;
                    entry->length = (end - entry->base);

                    found = true;
                    break;
                }
            } else {
                run_len = 0;
            }
        }
    }

    if (!buddy_page_array)
        k_panic("Failed to allocate buddy metadata");

    for (int i = 0; i < MAX_ORDER; i++) {
        buddy_free_area[i].next = NULL;
        buddy_free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++)
        buddy_add_entry(buddy_page_array, memmap->entries[i], buddy_free_area);

    global.buddy_active = true;
}

paddr_t pmm_alloc_page() {
    return pmm_alloc_pages(1);
}

static struct spinlock pmalloc_lock = SPINLOCK_INIT;
paddr_t pmm_alloc_pages(uint64_t count) {
    bool iflag = spin_lock(&pmalloc_lock);

    if (!global.buddy_active) {
        paddr_t p = bitmap_alloc_pages(count);
        spin_unlock(&pmalloc_lock, iflag);
        return p;
    }

    paddr_t p = buddy_alloc_pages(count);
    spin_unlock(&pmalloc_lock, iflag);
    return p;
}

void pmm_free_pages(paddr_t addr, uint64_t count) {
    bool iflag = spin_lock(&pmalloc_lock);
    if (!global.buddy_active) {
        bitmap_free_pages(addr, count);
        spin_unlock(&pmalloc_lock, iflag);
        return;
    }

    buddy_free_pages(addr, count);
    spin_unlock(&pmalloc_lock, iflag);
}

uint64_t pmm_get_usable_ram(void) {
    return global.total_pages * PAGE_SIZE;
}
