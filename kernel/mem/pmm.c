#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <misc/align.h>
#include <mp/domain.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct limine_memmap_response *memmap;
typedef paddr_t (*alloc_fn)(size_t pages, enum alloc_class class,
                            enum alloc_flags f);

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

static inline void *fast_memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;

    if (__builtin_constant_p(c) && c == 0) {
        size_t qwords = n / 8;
        size_t bytes = n % 8;

        if (qwords) {
            asm volatile("rep stosq"
                         : "+D"(d), "+c"(qwords)
                         : "a"(0ULL)
                         : "memory");
        }

        while (bytes--)
            *d++ = 0;

        return dst;
    }

    while (n--)
        *d++ = (unsigned char) c;

    return dst;
}

static void mid_init_buddy(size_t pages_needed) {
    bool found = false;

    for (uint64_t i = 0; i < memmap->entry_count && !found; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);
        uint64_t run_len = end > start ? (end - start) / PAGE_SIZE : 0;

        if (run_len >= pages_needed) {
            buddy_page_array = (void *) (start + global.hhdm_offset);
            fast_memset(buddy_page_array, 0, pages_needed * PAGE_SIZE);

            for (uint64_t j = 0; j < pages_needed; j++)
                set_bit((start / PAGE_SIZE) + j);

            entry->base = start + pages_needed * PAGE_SIZE;
            entry->length = end - entry->base;

            found = true;
            break;
        }
    }

    if (!buddy_page_array)
        k_panic("Failed to allocate buddy metadata");
}

void pmm_mid_init() {
    size_t pages_needed =
        (sizeof(struct buddy_page) * global.last_pfn + PAGE_SIZE - 1) /
        PAGE_SIZE;

    mid_init_buddy(pages_needed);

    for (int i = 0; i < MAX_ORDER; i++) {
        buddy_free_area[i].next = NULL;
        buddy_free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++)
        buddy_add_entry(buddy_page_array, memmap->entries[i], buddy_free_area);

    global.buddy_active = true;
    current_alloc_fn = buddy_alloc_pages_global;
    current_free_fn = buddy_free_pages_global;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        uint64_t start = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;

        if (e->type == LIMINE_MEMMAP_USABLE) {
            for (uint64_t p = start; p < end; p++)
                buddy_page_array[p].phys_usable = 1;
        }
    }
}

static void link_domain_cores_to_buddy(struct core_domain *cd,
                                       struct domain_buddy *bd) {
    for (size_t i = 0; i < cd->num_cores; i++) {
        cd->cores[i]->domain_buddy = bd;
    }
}

static inline uint64_t pages_to_bytes(size_t pages) {
    return (uint64_t) pages * PAGE_SIZE;
}

static void late_init_from_numa(size_t domain_count) {
    for (size_t i = 0; i < domain_count; i++) {
        struct numa_node *node = &global.numa_nodes[i % global.numa_node_count];
        struct core_domain *cd = global.core_domains[i];

        domain_buddies[i].start = node->mem_base;                /* bytes */
        domain_buddies[i].end = node->mem_base + node->mem_size; /* bytes */
        domain_buddies[i].length = node->mem_size;               /* bytes */
        domain_buddies[i].core_count = cd->num_cores;
        link_domain_cores_to_buddy(cd, &domain_buddies[i]);

        /* Slice of global buddy_page_array corresponding to this PFN range */
        size_t page_offset = node->mem_base / PAGE_SIZE; /* PFN index */
        domain_buddies[i].buddy = &buddy_page_array[page_offset];
    }
}

/* No NUMA: split last_pfn evenly across domains.
 * Keep dom->start/dom->end in bytes to match NUMA path. */
static void late_init_non_numa(size_t domain_count) {
    size_t pages_per_domain = global.last_pfn / domain_count;
    size_t remainder_pages = global.last_pfn % domain_count;

    size_t page_cursor = 0; /* PFN cursor (pages) */

    for (size_t i = 0; i < domain_count; i++) {
        size_t this_pages = pages_per_domain;
        if (i == domain_count - 1)
            this_pages += remainder_pages;

        struct core_domain *cd = global.core_domains[i];

        uint64_t domain_start_bytes = pages_to_bytes(page_cursor); /* bytes */

        uint64_t domain_length_bytes = pages_to_bytes(this_pages); /* bytes */

        domain_buddies[i].start = domain_start_bytes; /* bytes */
        domain_buddies[i].end =
            domain_start_bytes + domain_length_bytes;   /* bytes */
        domain_buddies[i].length = domain_length_bytes; /* bytes */
        domain_buddies[i].core_count = cd->num_cores;

        domain_buddies[i].buddy = &buddy_page_array[page_cursor];

        link_domain_cores_to_buddy(cd, &domain_buddies[i]);

        page_cursor += this_pages;
    }
}

/* We construct the buddies here, then
 * we hand it off to domain_buddies_init.
 *
 * This is just to avoid a bunch of NUMA
 * logic sitting around in the domain_buddies_init */
void pmm_late_init(void) {
    size_t domain_count = global.domain_count;
    domain_buddies = kzalloc(sizeof(struct domain_buddy) * domain_count);

    if (global.numa_node_count > 1) {
        late_init_from_numa(domain_count);
    } else {
        late_init_non_numa(domain_count);
    }

    domain_buddies_init();
    current_alloc_fn = domain_alloc;
    current_free_fn = domain_free;
}

paddr_t pmm_alloc_page(enum alloc_class c, enum alloc_flags f) {
    return pmm_alloc_pages(1, c, f);
}

void pmm_free_page(paddr_t addr) {
    pmm_free_pages(addr, 1);
}

paddr_t pmm_alloc_pages(uint64_t count, enum alloc_class c,
                        enum alloc_flags f) {
    paddr_t ret = current_alloc_fn(count, c, f);
    return ret;
}

void pmm_free_pages(paddr_t addr, uint64_t count) {
    current_free_fn(addr, count);
}

uint64_t pmm_get_usable_ram(void) {
    return global.total_pages * PAGE_SIZE;
}
