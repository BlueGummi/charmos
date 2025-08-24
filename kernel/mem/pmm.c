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
typedef paddr_t (*alloc_fn)(size_t pages);
typedef void (*free_fn)(paddr_t addr, size_t pages);

static alloc_fn current_alloc_fn = bitmap_alloc_pages;
static free_fn current_free_fn = bitmap_free_pages;

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

static void mid_init_buddy(size_t pages_needed) {
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
}

void pmm_mid_init() {
    size_t pages_needed =
        (sizeof(struct buddy_page) * global.total_pages + PAGE_SIZE - 1) /
        PAGE_SIZE;

    mid_init_buddy(pages_needed);
    if (!buddy_page_array)
        k_panic("Failed to allocate buddy metadata");

    for (int i = 0; i < MAX_ORDER; i++) {
        buddy_free_area[i].next = NULL;
        buddy_free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++)
        buddy_add_entry(buddy_page_array, memmap->entries[i], buddy_free_area);

    global.buddy_active = true;
    current_alloc_fn = buddy_alloc_pages_global;
    current_free_fn = buddy_free_pages_global;
}

static void late_init_from_numa(size_t domain_count) {
    for (size_t i = 0; i < domain_count; i++) {
        struct numa_node *node = &global.numa_nodes[i % global.numa_node_count];

        domain_buddies[i].start = node->mem_base;
        domain_buddies[i].end = node->mem_base + node->mem_size;

        domain_buddies[i].length = node->mem_size;

        /* Slice of buddy array corresponding to this range */
        size_t page_offset = node->mem_base / PAGE_SIZE;
        domain_buddies[i].buddy = &buddy_page_array[page_offset];
    }
}

/* No NUMA, just split evenly */
static void late_init_non_numa(size_t domain_count) {
    size_t pages_per_domain = global.total_pages / domain_count;
    size_t remainder_pages = global.total_pages % domain_count;

    uintptr_t base = 0;
    size_t page_cursor = 0;

    for (size_t i = 0; i < domain_count; i++) {
        size_t this_pages = pages_per_domain;
        if (i == domain_count - 1)
            this_pages += remainder_pages;

        domain_buddies[i].start = base;

        domain_buddies[i].end = base + this_pages / PAGE_SIZE;
        domain_buddies[i].length = this_pages / PAGE_SIZE;

        domain_buddies[i].buddy = &buddy_page_array[page_cursor];

        page_cursor += this_pages;
        base += this_pages / PAGE_SIZE;
    }
}

/* We construct the buddies here, then
 * we hand it off to domain_buddies_init.
 *
 * This is just to avoid a bunch of NUMA
 * logic sitting around in the domain_buddies_init */

void pmm_late_init(void) {
    size_t domain_count = global.domain_count;
    domain_buddies = kmalloc(sizeof(struct domain_buddy) * domain_count);

    if (global.numa_node_count > 1) {
        late_init_from_numa(domain_count);
    } else {
        late_init_non_numa(domain_count);
    }

    domain_buddies_init();
}

paddr_t pmm_alloc_page() {
    return pmm_alloc_pages(1);
}

void pmm_free_page(paddr_t addr) {
    pmm_free_pages(addr, 1);
}

static struct spinlock pmalloc_lock = SPINLOCK_INIT;
paddr_t pmm_alloc_pages(uint64_t count) {
    bool iflag = spin_lock(&pmalloc_lock);
    paddr_t p = current_alloc_fn(count);
    spin_unlock(&pmalloc_lock, iflag);
    return p;
}

void pmm_free_pages(paddr_t addr, uint64_t count) {
    bool iflag = spin_lock(&pmalloc_lock);
    current_free_fn(addr, count);
    spin_unlock(&pmalloc_lock, iflag);
}

uint64_t pmm_get_usable_ram(void) {
    return global.total_pages * PAGE_SIZE;
}
