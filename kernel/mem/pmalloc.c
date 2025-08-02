#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOOT_BITMAP_SIZE ((1024 * 1024 * 128) / PAGE_SIZE / 8)
#define MAX_ORDER 20

static uint8_t boot_bitmap[BOOT_BITMAP_SIZE];
static bool buddy_active = false;

static struct free_area free_area[MAX_ORDER] = {0};

static struct limine_memmap_response *memmap;
static uint64_t total_pages = 0;

static int fls(int mask) {
    int bit;

    if (mask == 0)
        return (0);
    for (bit = 1; mask != 1; bit++)
        mask = (unsigned int) mask >> 1;
    return (bit);
}

#define MIN(x, y) ((x) > (y) ? (y) : (x))

static void add_to_free_area(struct buddy_page *page, struct free_area *area) {
    page->free_area = area;
    page->next = area->next;
    area->next = page;
    area->nr_free++;
}

static struct buddy_page *remove_from_free_area(struct free_area *area) {
    if (area->nr_free == 0 || area->next == NULL)
        return NULL;

    struct buddy_page *page = area->next;
    area->next = page->next;
    area->nr_free--;
    return page;
}

static void do_add_entry(struct limine_memmap_entry *entry) {
    if (entry->type == LIMINE_MEMMAP_USABLE) {
        uint64_t start = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t end = (entry->base + entry->length) & ~(PAGE_SIZE - 1);

        uint64_t region_start = start / PAGE_SIZE;
        uint64_t region_size = (end - start) / PAGE_SIZE;

        for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
            uint64_t index = addr / PAGE_SIZE;
            if (index >= BOOT_BITMAP_SIZE * 8 && index < total_pages)
                clear_bit(index);
        }

        while (region_size > 0) {
            uint64_t order = MIN(fls(region_size) - 1, MAX_ORDER - 1);
            uint64_t block_size = 1 << order;

            struct buddy_page *page = kmalloc(sizeof(struct buddy_page));
            if (!page)
                k_panic("Failed to allocate buddy metadata");

            page->pfn = region_start;
            page->order = order;
            page->next = NULL;

            add_to_free_area(page, &free_area[order]);

            region_start += block_size;
            region_size -= block_size;
        }
    }
}

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

void pmm_dyn_init() {
    uint64_t size = total_pages / 8;
    uint8_t *new_bitmap = kmalloc(size);
    if (!new_bitmap)
        k_panic("Could not allocate space for physical memory allocator\n");

    uint64_t to_copy = BOOT_BITMAP_SIZE > size ? size : BOOT_BITMAP_SIZE;
    memcpy(new_bitmap, bitmap, to_copy);
    bitmap_size = total_pages / 8;
    bitmap = new_bitmap;

    for (int i = 0; i < MAX_ORDER; i++) {
        free_area[i].next = NULL;
        free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        do_add_entry(entry);
    }

    //buddy_active = true;
}

static void *buddy_alloc_pages(uint64_t count, bool add_offset) {
    if (count == 0)
        return NULL;

    uint64_t order = 0;
    uint64_t size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    if (order >= MAX_ORDER)
        return NULL;

    uint64_t current_order = order;
    while (current_order < MAX_ORDER && free_area[current_order].nr_free == 0) {
        current_order++;
    }

    if (current_order >= MAX_ORDER)
        return NULL;

    while (current_order > order) {
        struct buddy_page *page =
            remove_from_free_area(&free_area[current_order]);
        if (!page)
            return NULL;

        uint64_t new_order = current_order - 1;
        uint64_t buddy_pfn = page->pfn + (1 << new_order);

        struct buddy_page *buddy = kmalloc(sizeof(struct buddy_page));
        if (!buddy) {
            add_to_free_area(page, &free_area[current_order]);
            return NULL;
        }

        page->order = new_order;

        buddy->pfn = buddy_pfn;
        buddy->order = new_order;
        buddy->next = NULL;

        add_to_free_area(page, &free_area[new_order]);
        add_to_free_area(buddy, &free_area[new_order]);

        current_order--;
    }

    struct buddy_page *page = remove_from_free_area(&free_area[order]);
    if (!page)
        return NULL;

    void *addr = (void *) (page->pfn * PAGE_SIZE);
    kfree(page);

    return add_offset ? (void *) ((uintptr_t) addr + global.hhdm_offset) : addr;
}

static void buddy_free_pages(void *addr, uint64_t count, bool has_offset) {
    if (!addr || count == 0)
        return;

    uint64_t pfn =
        ((uintptr_t) addr - (has_offset ? global.hhdm_offset : 0)) / PAGE_SIZE;
    uint64_t order = 0;
    uint64_t size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    struct buddy_page *page = kmalloc(sizeof(struct buddy_page));
    if (!page)
        return;

    page->pfn = pfn;
    page->order = order;
    page->next = NULL;

    while (order < MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ (1 << order);

        bool buddy_found = false;
        struct buddy_page *buddy = free_area[order].next;
        struct buddy_page *prev = NULL;

        while (buddy) {
            if (buddy->pfn == buddy_pfn) {
                buddy_found = true;
                break;
            }
            prev = buddy;
            buddy = buddy->next;
        }

        if (!buddy_found)
            break;

        if (prev) {
            prev->next = buddy->next;
        } else {
            free_area[order].next = buddy->next;
        }
        free_area[order].nr_free--;

        pfn &= buddy_pfn;
        order++;

        page->pfn = pfn;
        page->order = order;

        kfree(buddy);
    }

    add_to_free_area(page, &free_area[order]);
}

void *pmm_alloc_page(bool add_offset) {
    return pmm_alloc_pages(1, add_offset);
}

void *pmm_alloc_pages(uint64_t count, bool add_offset) {
    if (!buddy_active)
        return bitmap_alloc_pages(count, add_offset);

    return buddy_alloc_pages(count, add_offset);
}

void pmm_free_pages(void *addr, uint64_t count, bool has_offset) {
    if (!buddy_active)
        return bitmap_free_pages(addr, count, has_offset);

    buddy_free_pages(addr, count, has_offset);
}

uint64_t pmm_get_usable_ram(void) {
    return total_pages * PAGE_SIZE;
}
