#include <console/printf.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/domain.h>
#include <mem/pmm.h>
#include <smp/domain.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct limine_memmap_response *memmap;
typedef paddr_t (*alloc_fn)(size_t pages, enum alloc_flags f);

typedef void (*free_fn)(paddr_t addr, size_t pages);

static alloc_fn current_alloc_fn = bitmap_alloc_pages;
static free_fn current_free_fn = bitmap_free_pages;

__no_sanitize_address void pmm_early_init(struct limine_memmap_request m) {
    bitmap = boot_bitmap;
    memmap = m.response;

    if (memmap == NULL || memmap->entries == NULL) {
        panic("Failed to retrieve Limine memory map\n");
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

    uint64_t last_usable_pfn = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t end =
            ALIGN_UP(entry->base + entry->length, PAGE_SIZE) / PAGE_SIZE;

        if (end > last_usable_pfn)
            last_usable_pfn = end;
    }

    global.last_pfn = last_usable_pfn;
    global.total_pages = total_phys / PAGE_SIZE;
}

__no_sanitize_address void pmm_mid_init() {
    buddy_init();
    current_alloc_fn = buddy_alloc_pages_global;
    current_free_fn = buddy_free_pages_global;
}

void pmm_late_init(void) {
    domain_buddies_init();
    current_alloc_fn = domain_alloc;
    current_free_fn = domain_free;
}

paddr_t pmm_alloc_page(enum alloc_flags f) {
    return pmm_alloc_pages(1, f);
}

void pmm_free_page(paddr_t addr) {
    pmm_free_pages(addr, 1);
}

paddr_t pmm_alloc_pages(uint64_t count, enum alloc_flags f) {
    return current_alloc_fn(count, f);
}

void pmm_free_pages(paddr_t addr, uint64_t count) {
    current_free_fn(addr, count);
}

uint64_t pmm_get_usable_ram(void) {
    return global.total_pages * PAGE_SIZE;
}
