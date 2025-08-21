#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <misc/align.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOOT_BITMAP_SIZE ((1024 * 1024 * 128) / PAGE_SIZE / 8)
#define MAX_ORDER 20
#define MIN(x, y) ((x) > (y) ? (y) : (x))

static uint8_t boot_bitmap[BOOT_BITMAP_SIZE];
static bool buddy_active = false;
static struct free_area free_area[MAX_ORDER] = {0};
static struct buddy_page *buddy_page_array = NULL;

static struct limine_memmap_response *memmap;
static uint64_t total_pages = 0;

static bool pfn_usable_from_memmap(uint64_t pfn) {
    uint64_t addr = pfn * PAGE_SIZE;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = ALIGN_DOWN(entry->base, PAGE_SIZE);

        uint64_t end = ALIGN_UP(entry->base + entry->length, PAGE_SIZE);

        if (addr >= start && addr < end)

            return true;
    }
    return false;
}

static bool is_block_free(uint64_t pfn, uint64_t order) {
    uint64_t pages = 1ULL << order;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t cur_pfn = pfn + i;

        if (cur_pfn < BOOT_BITMAP_SIZE * 8) {
            if (test_bit(cur_pfn))
                return false;
        } else {
            if (!pfn_usable_from_memmap(cur_pfn))
                return false;
        }
    }
    return true;
}

static inline int order_base_2(uint64_t x) {
    return 64 - __builtin_clzll(x) - 1;
}

static void add_to_free_area(struct buddy_page *page, struct free_area *area) {
    page->free_area = area;
    page->next = area->next;
    area->next = page;
    area->nr_free++;
    page->is_free = true;
}

static struct buddy_page *remove_from_free_area(struct free_area *area) {
    if (area->nr_free == 0 || area->next == NULL)
        return NULL;

    struct buddy_page *page = area->next;
    area->next = page->next;
    area->nr_free--;
    page->is_free = false;
    return page;
}

static void do_add_entry(struct limine_memmap_entry *entry) {
    if (entry->type != LIMINE_MEMMAP_USABLE)
        return;

    uint64_t start = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end = (entry->base + entry->length) & ~(PAGE_SIZE - 1);

    if (start >= end)
        return;

    uint64_t region_start = start / PAGE_SIZE;
    uint64_t region_size = (end - start) / PAGE_SIZE;

    while (region_size > 0) {
        uint64_t order = MIN(order_base_2(region_size), MAX_ORDER - 1);
        uint64_t block_size = 1ULL << order;

        while ((region_start & (block_size - 1)) != 0 && order > 0) {
            order--;
            block_size = 1ULL << order;
        }

        if (region_start + block_size > total_pages)
            break;

        if (is_block_free(region_start, order)) {
            struct buddy_page *page = &buddy_page_array[region_start];
            memset(page, 0, sizeof(*page));
            page->pfn = region_start;
            page->order = order;
            add_to_free_area(page, &free_area[order]);
        }

        region_start += block_size;
        region_size -= block_size;
    }
    buddy_active = true;
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
    total_pages = total_phys / PAGE_SIZE;
}

void pmm_dyn_init() {
    buddy_page_array = kzalloc(sizeof(struct buddy_page) * total_pages);
    if (!buddy_page_array)
        k_panic("Failed to allocate buddy metadata");

    for (int i = 0; i < MAX_ORDER; i++) {
        free_area[i].next = NULL;
        free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++)
        do_add_entry(memmap->entries[i]);

    buddy_active = true;
}

static void *buddy_alloc_pages(uint64_t count, bool add_offset) {
    if (count == 0)
        k_panic("Tried to allocate zero pages\n");

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    if (order >= MAX_ORDER) {
        return NULL;
    }

    uint64_t current_order = order;
    while (current_order < MAX_ORDER && free_area[current_order].nr_free == 0)
        current_order++;

    if (current_order >= MAX_ORDER) {
        return NULL;
    }

    while (current_order > order) {
        struct buddy_page *page =
            remove_from_free_area(&free_area[current_order]);
        if (!page) {
            return NULL;
        }

        uint64_t new_order = current_order - 1;
        uint64_t buddy_pfn = page->pfn + (1ULL << new_order);

        struct buddy_page *buddy = &buddy_page_array[buddy_pfn];
        memset(buddy, 0, sizeof(*buddy));

        page->order = new_order;
        buddy->pfn = buddy_pfn;
        buddy->order = new_order;

        add_to_free_area(page, &free_area[new_order]);
        add_to_free_area(buddy, &free_area[new_order]);

        current_order--;
    }

    struct buddy_page *page = remove_from_free_area(&free_area[order]);
    if (!page) {
        return NULL;
    }

    return (void *) ((page->pfn * PAGE_SIZE) +
                     (add_offset ? global.hhdm_offset : 0));
}

static void buddy_free_pages(void *addr, uint64_t count, bool has_offset) {
    if (!addr || count == 0)
        return;

    uint64_t pfn =
        ((uintptr_t) addr - (has_offset ? global.hhdm_offset : 0)) / PAGE_SIZE;
    if (pfn >= total_pages)
        return;

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    struct buddy_page *page = &buddy_page_array[pfn];
    memset(page, 0, sizeof(*page));
    page->pfn = pfn;
    page->order = order;

    while (order < MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ (1ULL << order);

        if (buddy_pfn >= total_pages)
            break;

        struct buddy_page *buddy = &buddy_page_array[buddy_pfn];
        if (!buddy->is_free || buddy->order != order)
            break;

        struct buddy_page **prev = &free_area[order].next;
        while (*prev && *prev != buddy) {
            prev = &(*prev)->next;
        }
        if (*prev == buddy) {
            *prev = buddy->next;
            free_area[order].nr_free--;
        }

        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        page = &buddy_page_array[pfn];
        memset(page, 0, sizeof(*page));
        page->pfn = pfn;
        page->order = ++order;
    }

    add_to_free_area(page, &free_area[order]);
}

void *pmm_alloc_page(bool add_offset) {
    return pmm_alloc_pages(1, add_offset);
}

static struct spinlock pmalloc_lock = SPINLOCK_INIT;
void *pmm_alloc_pages(uint64_t count, bool add_offset) {
    bool iflag = spin_lock(&pmalloc_lock);

    if (!buddy_active) {
        void *p = bitmap_alloc_pages(count, add_offset);
        spin_unlock(&pmalloc_lock, iflag);
        return p;
    }

    void *p = buddy_alloc_pages(count, add_offset);
    spin_unlock(&pmalloc_lock, iflag);
    return p;
}

void pmm_free_pages(void *addr, uint64_t count, bool has_offset) {
    bool iflag = spin_lock(&pmalloc_lock);
    if (!buddy_active) {
        bitmap_free_pages(addr, count, has_offset);
        spin_unlock(&pmalloc_lock, iflag);
        return;
    }

    buddy_free_pages(addr, count, has_offset);
    spin_unlock(&pmalloc_lock, iflag);
}

uint64_t pmm_get_usable_ram(void) {
    return total_pages * PAGE_SIZE;
}
