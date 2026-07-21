#include <acpi/lapic.h>
#include <cmdline.h>
#include <console/printf.h>
#include <global.h>
#include <irq/idt.h>
#include <limine.h>
#include <linker/symbols.h>
#include <mem/address_range.h>
#include <mem/asan.h>
#include <mem/demand_page.h>
#include <mem/hhdm.h>
#include <mem/page_table.h>
#include <mem/pmm.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/smp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "mem/slab/internal.h"

#define KERNEL_PML4_START_INDEX 256

static inline bool in_use(pte_t pte) {
    return pte_in_use((pte_atomic_t *) &pte);
}

static void vmm_mem_cmdline_callback(const char *str,
                                     struct cmdline_entry *ent);

enum pt_level {
    PT_LEVEL_PML4 = 0,
    PT_LEVEL_PDPT = 1,
    PT_LEVEL_PD = 2,
    PT_LEVEL_PT = 3,
};

struct pt_deferred_free {
    paddr_t phys;
    uint64_t epoch;
    struct pt_deferred_free *next;
};

struct pt_walk {
    struct page_table *tables[PT_LEVELS];
    pte_t *entries[PT_LEVELS - 1];
    enum irql irqls[PT_LEVELS - 1];
    int depth;
};

CMDLINE_ENTRY_DECLARE(mem,
                      .desc = "Cap on physical memory the allocator will use",
                      .arg = "<hex bytes>",
                      .callback = vmm_mem_cmdline_callback,
                      .default_val = "0x700000000000",
                      .flags = CMDLINE_ENTRY_FLAGS_NONE, .value = NULL);

ADDRESS_RANGE_DECLARE(hhdm, .base = 0xFFFF800000000000ULL,
                      /* default size: from the base up to the slab heap */
                      .size = SLAB_HEAP_START - 0xFFFF800000000000ULL);

ADDRESS_RANGE_DECLARE(kernel, .base = 0xffffffff80000000,
                      .size = 0); /* Filled in at boot */

bool hhdm_vaddr_in_range(vaddr_t vaddr) {
    return address_range_addr_in_range(&ADDRESS_RANGE(hhdm), vaddr);
}

bool hhdm_paddr_in_range(paddr_t paddr) {
    return paddr < ADDRESS_RANGE(hhdm).size;
}

bool hhdm_ptr_in_range(void *ptr) {
    return hhdm_vaddr_in_range((vaddr_t) ptr);
}

static struct pt_deferred_free *pt_free_list;
static struct spinlock pt_free_lock = SPINLOCK_INIT;
static struct page_table *kernel_pml4 = NULL;
static uintptr_t vmm_map_top = VMM_MAP_BASE;

static long string_to_int(const char *str) {
    char *endptr;

    if (str[0] == '0' && (str[1] == 'b' || str[1] == 'B')) {
        return strtol(str + 2, &endptr, 2);
    }

    return strtol(str, &endptr, 0);
}

static void vmm_mem_cmdline_callback(const char *str,
                                     struct cmdline_entry *ent) {
    ADDRESS_RANGE(hhdm).size = string_to_int(str);
}

static inline struct page_table *alloc_pt(void) {
    paddr_t phys = pmm_alloc_page();
    if (!phys)
        return NULL;

    void *virt = hhdm_paddr_to_ptr(phys);
    memset(virt, 0, PAGE_SIZE);
    return virt;
}

static enum errno pte_init(pte_t *entry, uint64_t flags) {
    struct page_table *new_table = alloc_pt();
    if (!new_table)
        return ERR_NO_MEM;

    uintptr_t new_table_phys = hhdm_ptr_to_paddr(new_table);
    *entry = new_table_phys | PAGE_PRESENT | PAGE_WRITE | PTE_LOCK_BIT | flags;
    return ERR_OK;
}

enum irql pte_lock(pte_t *pt) {
    return pte_lock_irql((void *) pt);
}

void pte_unlock(pte_t *pt, enum irql irql) {
    pte_unlock_irql((void *) pt, irql);
}

static void barrier_and_shootdown(enum vmm_flags flags, vaddr_t virt) {
    memory_barrier();
    invlpg(virt);

    if (!(flags & VMM_FLAG_NO_TLB_SHOOTDOWN))
        tlb_shootdown(virt, true);
}

static inline uint64_t pt_index(uintptr_t virt, int level) {
    return (virt >> (PT_SHIFT_L4 - level * PT_STRIDE)) & PT_INDEX_MASK;
}

static inline struct page_table *pt_next_table(pte_t entry) {
    return hhdm_paddr_to_ptr(entry & PAGE_PHYS_MASK);
}

static inline void pt_walk_enter(void) {
    if (global.current_bootstage >= BOOTSTAGE_MID_MP) {
        uint64_t e =
            atomic_load_explicit(&global.pt_epoch, memory_order_acquire);
        atomic_store_explicit(&smp_core()->pt_seen_epoch, e,
                              memory_order_release);
    }
}

static inline void pt_walk_exit(void) {
    if (global.current_bootstage >= BOOTSTAGE_MID_MP) {
        atomic_store_explicit(&smp_core()->pt_seen_epoch, UINT64_MAX,
                              memory_order_release);
    }
}

static void enqueue_pt_free(paddr_t phys) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return pmm_free_page(phys);

    struct pt_deferred_free *n =
        (struct pt_deferred_free *) hhdm_paddr_to_ptr(phys);

    uint64_t e =
        atomic_fetch_add_explicit(&global.pt_epoch, 1, memory_order_acq_rel) +
        1;
    n->phys = phys;
    n->epoch = e;

    enum irql irql = spin_lock_irq_disable(&pt_free_lock);
    n->next = pt_free_list;
    pt_free_list = n;
    spin_unlock(&pt_free_lock, irql);
}

void vmm_reclaim_page_tables(void) {
    if (global.current_bootstage < BOOTSTAGE_LATE)
        return;

    if (smp_core()->reclaiming_page_tables)
        return;

    kassert(irql_get() == IRQL_DISPATCH_LEVEL);
    smp_core()->reclaiming_page_tables = true;

    uint64_t min_epoch = UINT64_MAX;
    struct core *cpu;
    for_each_cpu_struct(cpu) {
        uint64_t seen =
            atomic_load_explicit(&cpu->pt_seen_epoch, memory_order_acquire);
        if (seen < min_epoch)
            min_epoch = seen;
    }

    enum irql irql = IRQL_NONE;
    if (!spin_trylock_irq_disable(&pt_free_lock, &irql))
        goto out;

    struct pt_deferred_free *to_free = NULL;
    struct pt_deferred_free **pp = &pt_free_list;
    while (*pp) {
        struct pt_deferred_free *n = *pp;
        if (n->epoch >= min_epoch) {
            pp = &n->next;
        } else {
            *pp = n->next;
            n->next = to_free;
            to_free = n;
        }
    }

    spin_unlock(&pt_free_lock, irql);

    while (to_free) {
        struct pt_deferred_free *n = to_free;
        paddr_t phys = n->phys;
        to_free = n->next;
        pmm_free_pages(phys, 1);
    }

out:
    smp_core()->reclaiming_page_tables = false;
}

uintptr_t vmm_make_user_pml4(void) {
    struct page_table *user_pml4 = alloc_pt();
    if (!user_pml4) {
        panic("Failed to allocate user pml4");
    }

    for (int i = KERNEL_PML4_START_INDEX; i < PT_ENTRIES; i++) {
        user_pml4->entries[i] = kernel_pml4->entries[i];
    }

    return hhdm_ptr_to_paddr(user_pml4);
}

/* Leaf frames are not touched here, this gets rid of structural page tables */
static void vmm_free_user_subtree(struct page_table *pdpt) {
    for (int i3 = 0; i3 < PT_ENTRIES; i3++) {
        pte_t e3 = pdpt->entries[i3];
        if (!in_use(e3) || (e3 & PAGE_PAGE_SIZE))
            continue;

        struct page_table *pd = pt_next_table(e3);
        for (int i2 = 0; i2 < PT_ENTRIES; i2++) {
            pte_t e2 = pd->entries[i2];
            if (!in_use(e2) || (e2 & PAGE_2MB_page))
                continue;

            struct page_table *pt = pt_next_table(e2);
            enqueue_pt_free(hhdm_ptr_to_paddr(pt));
        }
        enqueue_pt_free(hhdm_ptr_to_paddr(pd));
    }
}

void vmm_unmap_all_user_pages(struct page_table *pml4, enum vmm_flags vflags) {
    (void) vflags;

    for (int i4 = 0; i4 < KERNEL_PML4_START_INDEX; i4++) {
        pte_t e4 = pml4->entries[i4];
        if (!in_use(e4))
            continue;

        struct page_table *pdpt = pt_next_table(e4);
        vmm_free_user_subtree(pdpt);
        enqueue_pt_free(hhdm_ptr_to_paddr(pdpt));
        pml4->entries[i4] = 0;
    }
}

void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa) {
    kernel_pml4 = alloc_pt();
    if (!kernel_pml4)
        panic("Could not allocate space for kernel PML4");

    uintptr_t kernel_pml4_phys = hhdm_ptr_to_paddr(kernel_pml4);

    uint64_t kernel_phys_start = xa->physical_base;
    uint64_t kernel_virt_start = xa->virtual_base;
    uint64_t kernel_virt_end = (uint64_t) &__kernel_virt_end;
    uint64_t kernel_size = kernel_virt_end - kernel_virt_start;
    ADDRESS_RANGE(kernel).size = kernel_size;

    paddr_t dummy_phys = pmm_alloc_page();
    uint8_t *dummy_virt = hhdm_paddr_to_ptr(dummy_phys);
    memset(dummy_virt, 0xFF, PAGE_SIZE);

    enum errno e;

    /* We leave the kernel writable to do boot time patching, but
     * TODO: lock the kernel down and make it RO later on */
    for (uint64_t i = 0; i < kernel_size; i += PAGE_SIZE) {
        e = vmm_map_page(kernel_virt_start + i, kernel_phys_start + i,
                         PAGE_WRITE | PAGE_PRESENT, VMM_FLAG_NONE);
        if (e < 0)
            panic("Error %s whilst mapping kernel", errno_to_str(e));
    }

#ifdef DEBUG_ASAN
    for (uintptr_t addr = kernel_virt_start; addr < kernel_virt_end;
         addr += PAGE_SIZE) {
        uint64_t shadow_addr = ASAN_SHADOW_OFFSET + (addr >> ASAN_SHADOW_SCALE);
        shadow_addr = PAGE_ALIGN_DOWN(shadow_addr);
        vmm_map_page(shadow_addr, dummy_phys, PAGE_PRESENT | PAGE_WRITE,
                     VMM_FLAG_MODIFY_LEAF);
    }
#endif

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_BAD_MEMORY ||
            entry->type == LIMINE_MEMMAP_RESERVED ||
            entry->type == LIMINE_MEMMAP_ACPI_NVS) {
            continue;
        }

        uint64_t base = entry->base;
        uint64_t len = entry->length;
        uint64_t end = base + len;
        uint64_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_XD;

        if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            flags |= PAGE_WRITETHROUGH;
        }

        uint64_t phys = base;
        while (phys < end) {
            uint64_t virt = hhdm_paddr_to_vaddr(phys);

            bool can_use_2mb = ((phys % PAGE_2MB) == 0) &&
                               ((virt % PAGE_2MB) == 0) &&
                               ((end - phys) >= PAGE_2MB);

            if (can_use_2mb) {
                e = vmm_map_page(virt, phys, flags, VMM_FLAG_NONE,
                                 VMM_MAP_PAGE_SIZE_2MB);
                phys += PAGE_2MB;
            } else {
                e = vmm_map_page(virt, phys, flags);
                phys += PAGE_SIZE;
            }
            if (e < 0)
                panic("Error %s whilst mapping kernel", errno_to_str(e));
        }
    }

    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

static inline bool vmm_is_table_empty(struct page_table *table) {
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (in_use(table->entries[i]))
            return false;
    }
    return true;
}

static inline int map_leaf_level(enum vmm_map_page_size sz) {
    switch (sz) {
    case VMM_MAP_PAGE_SIZE_1GB: return PT_LEVEL_PDPT;

    case VMM_MAP_PAGE_SIZE_2MB: return PT_LEVEL_PD;

    default: return PT_LEVEL_PT;
    }
}

static inline size_t map_page_bytes(enum vmm_map_page_size sz) {
    switch (sz) {
    case VMM_MAP_PAGE_SIZE_1GB: return PAGE_1GB;
    case VMM_MAP_PAGE_SIZE_2MB: return PAGE_2MB;
    default: return PAGE_SIZE;
    }
}

static inline pte_t build_leaf_pte(paddr_t phys, uint64_t flags,
                                   enum vmm_map_page_size sz,
                                   enum vmm_flags vflags) {
    uint64_t extra_flags = (vflags & VMM_FLAG_MODIFY_LEAF) ? 0 : PAGE_PRESENT;
    if (sz != VMM_MAP_PAGE_SIZE_4KB && !(vflags & VMM_FLAG_MODIFY_LEAF))
        extra_flags |= PAGE_2MB_page;

    flags |= extra_flags;
    pte_t leaf = (phys & PAGE_PHYS_MASK) | flags;
    return leaf;
}

static enum errno vmm_pt_apply(struct vmm_map_request *rq) {
    bool reclaim = rq->is_unmap_internal;
    vaddr_t virt = rq->virt;
    if (virt == 0)
        panic("CANNOT MAP PAGE 0x0!!!");

    struct page_table *pml4 = rq->pml4 ? rq->pml4 : kernel_pml4;
    enum vmm_flags vflags = rq->vmm_flags;
    enum vmm_map_page_size sz = rq->page_size;
    int leaf_level = map_leaf_level(sz);
    bool want_huge = sz != VMM_MAP_PAGE_SIZE_4KB;

    bool clear = vflags & VMM_FLAG_CLEAR_LEAF;
    bool modify = vflags & VMM_FLAG_MODIFY_LEAF;
    bool handle_exist = vflags & VMM_FLAG_HANDLE_PTE_EXISTING;

    uint64_t user_flag =
        ((vflags & VMM_FLAG_USER) && !modify) ? PAGE_USER_ALLOWED : 0;
    uint64_t flags = rq->page_flags | user_flag;

    size_t bytes = map_page_bytes(sz);
    if (!IS_ALIGNED(virt, bytes) || (!clear && !IS_ALIGNED(rq->phys, bytes)))
        panic("vmm_pt_apply: huge mapping not naturally aligned");

    pt_walk_enter();
    enum errno err = ERR_OK;
    struct page_table *tables[PT_LEVELS];
    enum irql irqls[PT_LEVELS - 1];
    pte_t *entries[PT_LEVELS - 1];
    paddr_t to_free[PT_LEVELS - 1] = {0};
    int free_count = 0;

    tables[0] = pml4;

    int level = 0;
    for (level = 0; level < leaf_level; level++) {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

        pte_t *entry = &tables[level]->entries[pt_index(virt, level)];

#pragma GCC diagnostic pop

        entries[level] = entry;
        irqls[level] = pte_lock(entry);

        if (!in_use(*entry)) {
            /* clear/unmap never builds tables: no table here means no leaf */
            if (clear) {
                level++;
                goto out;
            }
            if ((err = pte_init(entry, user_flag)) < 0) {
                level++;
                goto out;
            }
        }

        /* present huge leaf where table was expected is a size mismatch
         *
         * only meaningful when present as non-present can have payload there */
        kassert(!((*entry & PAGE_PRESENT) && (*entry & PAGE_2MB_page)));
        tables[level + 1] = pt_next_table(*entry);
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

    pte_t *last_entry =
        &tables[leaf_level]->entries[pt_index(virt, leaf_level)];

#pragma GCC diagnostic pop

    enum irql last_irql = pte_lock(last_entry);

    bool was_present = *last_entry & PAGE_PRESENT;

    /* page tables must match the caller's claims, PS bit is only meaningful
     * for present bits as tagged non-present PTEs use bit 7 */
    if (was_present) {
        kassert((bool) (*last_entry & PAGE_2MB_page) == want_huge);
        if (handle_exist) {
            err = ERR_EXIST;
            pte_unlock(last_entry, last_irql);
            goto out;
        }
    }

    if (clear) {
        if (was_present)
            barrier_and_shootdown(vflags, virt);
        *last_entry = PTE_LOCK_BIT; /* zero all but the held lock bit */
    } else {
        if (in_use(*last_entry) && !modify)
            panic(
                "vmm_pt_apply: leaf in use without MODIFY_LEAF (double map?)");

        if (was_present)
            barrier_and_shootdown(vflags, virt);

        *last_entry =
            build_leaf_pte(rq->phys, flags, sz, vflags) | PTE_LOCK_BIT;
    }

    pte_unlock(last_entry, last_irql);

    /* used in unmap */
    if (reclaim) {
        for (int up = leaf_level; up > 0; up--) {
            if (!vmm_is_table_empty(tables[up]))
                break;
            to_free[free_count++] = hhdm_ptr_to_paddr(tables[up]);
            *entries[up - 1] = PTE_LOCK_BIT;
        }
    }

out:
    for (int i = level - 1; i >= 0; i--)
        pte_unlock(entries[i], irqls[i]);

    for (int i = 0; i < free_count; i++)
        enqueue_pt_free(to_free[i]);

    pt_walk_exit();
    return err;
}

enum errno vmm_map_page_full(struct vmm_map_request *rq) {
    return vmm_pt_apply(rq);
}

/* Tear down a single leaf and reclaim every page table it leaves empty. */
void vmm_unmap_page_full(struct vmm_map_request *rq) {
    struct vmm_map_request req = *rq;
    req.vmm_flags |= VMM_FLAG_CLEAR_LEAF;
    req.is_unmap_internal = true;
    (void) vmm_pt_apply(&req);
}

enum errno vmm_map_page_internal(vaddr_t virt, paddr_t phys, page_flags_t flags,
                                 enum vmm_flags vflags,
                                 enum vmm_map_page_size size) {
    struct vmm_map_request rq = {
        .virt = virt,
        .phys = phys,
        .page_flags = flags,
        .vmm_flags = vflags,
        .page_size = size,
    };
    return vmm_map_page_full(&rq);
}

enum errno vmm_map_page_user_internal(struct page_table *pml4, vaddr_t virt,
                                      paddr_t phys, page_flags_t flags,
                                      enum vmm_flags vflags,
                                      enum vmm_map_page_size size) {
    struct vmm_map_request rq = {
        .pml4 = pml4,
        .virt = virt,
        .phys = phys,
        .page_flags = flags,
        .vmm_flags = vflags | VMM_FLAG_USER,
        .page_size = size,
    };
    return vmm_map_page_full(&rq);
}

enum errno vmm_mark_demand_page_internal(vaddr_t virt,
                                         enum demand_page_flags flags,
                                         enum vmm_map_page_size size) {
    struct pte_tagged ptag = {
        .type = PTE_TAG_TYPE_DEMAND_PAGED,
        .payload = flags,
    };

    uint64_t packed = pte_tagged_pack(&ptag);

    struct vmm_map_request rq = {
        .pml4 = kernel_pml4,
        .virt = virt,
        .phys = 0,
        .page_flags = packed,
        .vmm_flags = VMM_FLAG_MODIFY_LEAF,
        .page_size = size,
    };

    return vmm_map_page_full(&rq);
}

enum errno vmm_map_demand_page_internal(vaddr_t virt, paddr_t phys,
                                        enum demand_page_flags flags,
                                        enum vmm_map_page_size size) {
    uint64_t pflags = PAGE_PRESENT;
    if (flags & DEMAND_PAGE_FLAG_WRITABLE)
        pflags |= PAGE_WRITE;

    if (flags & DEMAND_PAGE_FLAG_XD)
        pflags |= PAGE_XD;

    struct vmm_map_request rq = {
        .pml4 = kernel_pml4,
        .virt = virt,
        .phys = phys,
        .page_flags = pflags,
        .vmm_flags = VMM_FLAG_MODIFY_LEAF | VMM_FLAG_HANDLE_PTE_EXISTING |
                     VMM_FLAG_NO_TLB_SHOOTDOWN,
        .page_size = size,
    };

    return vmm_map_page_full(&rq);
}

enum errno vmm_mark_demand_page_user_internal(struct page_table *pml4,
                                              vaddr_t virt,
                                              enum demand_page_flags flags,
                                              enum vmm_map_page_size size) {
    struct pte_tagged ptag = {
        .type = PTE_TAG_TYPE_DEMAND_PAGED,
        .payload = flags,
    };

    uint64_t packed = pte_tagged_pack(&ptag);

    struct vmm_map_request rq = {
        .pml4 = pml4,
        .virt = virt,
        .phys = 0,
        .page_flags = packed,

        /* USER here is merely nominal, map_page_full ignores it */
        .vmm_flags = VMM_FLAG_USER | VMM_FLAG_MODIFY_LEAF,
        .page_size = size,
    };

    return vmm_map_page_full(&rq);
}

void vmm_unmap_page_internal(vaddr_t virt, enum vmm_flags vflags,
                             enum vmm_map_page_size size) {
    struct vmm_map_request rq = {
        .virt = virt,
        .vmm_flags = vflags,
        .page_size = size,
    };
    vmm_unmap_page_full(&rq);
}

static pte_t vmm_walk_leaf(struct page_table *root, vaddr_t virt,
                           int *out_level) {
    pt_walk_enter();

    struct page_table *table = root;
    uint64_t snap = 0;
    int level;

    for (level = 0; level < PT_LEVEL_PT; level++) {
        uint64_t index = pt_index(virt, level);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

        /* assert so that clang inlines this instead of creating a call to
         * __atomic_load (from c lib, breaks under -nostdlib), gcc inlines */
        snap = atomic_load_explicit((_Atomic pte_t *) __builtin_assume_aligned(
                                        &table->entries[index], sizeof(pte_t)),
                                    memory_order_acquire);

#pragma GCC diagnostic pop

        if (!(snap & PAGE_PRESENT)) {
            snap = 0;
            goto out;
        }

        if (snap & PAGE_2MB_page)
            goto out;

        table = pt_next_table(snap);
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

    snap = atomic_load_explicit(
        (_Atomic pte_t *) __builtin_assume_aligned(
            &table->entries[pt_index(virt, PT_LEVEL_PT)], sizeof(pte_t)),
        memory_order_acquire);

#pragma GCC diagnostic pop

out:
    pt_walk_exit();

    if (out_level)
        *out_level = level;

    return snap;
}

paddr_t vmm_get_phys_internal(uintptr_t virt, enum vmm_flags vflags) {
    (void) vflags;

    int level;
    uint64_t snap = vmm_walk_leaf(kernel_pml4, virt, &level);

    if (!(snap & PAGE_PRESENT))
        return (uintptr_t) -1;

    if (level == PT_LEVEL_PDPT)
        return (snap & PAGE_PHYS_MASK) + (virt & (PAGE_1GB - 1));

    if (level == PT_LEVEL_PD)
        return (snap & PAGE_2MB_PHYS_MASK) + (virt & (PAGE_2MB - 1));

    return (snap & PAGE_PHYS_MASK) + (virt & 0xFFF);
}

pte_t vmm_get_leaf_pte_internal(vaddr_t virt, enum vmm_flags vflags) {
    (void) vflags;
    return vmm_walk_leaf(kernel_pml4, virt, NULL);
}

void *vmm_map(paddr_t paddr, vaddr_t vaddr, uint64_t len, uint64_t flags,
              enum vmm_flags vflags) {
    if (len == 0)
        return NULL;

    uintptr_t phys_start = PAGE_ALIGN_DOWN(paddr);
    uintptr_t offset = paddr - phys_start;

    uint64_t total_len = len + offset;
    uint64_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    enum errno e = ERR_OK;
    uint64_t mapped = 0;

    for (; mapped < total_pages; mapped++) {
        e = vmm_map_page(vaddr + mapped * PAGE_SIZE,
                         phys_start + mapped * PAGE_SIZE,
                         PAGE_PRESENT | PAGE_WRITE | flags, vflags);
        if (e < 0)
            goto unwind;
    }

    return (void *) (vaddr + offset);

unwind:
    for (uint64_t i = 0; i < mapped; i++)
        vmm_unmap_page(vaddr + i * PAGE_SIZE, vflags);

    return NULL;
}

void vmm_unmap(void *addr, uint64_t len, enum vmm_flags vflags) {
    uintptr_t virt_addr = (uintptr_t) addr;
    uintptr_t page_offset = virt_addr & (PAGE_SIZE - 1);
    uintptr_t aligned_virt = PAGE_ALIGN_DOWN(virt_addr);

    uint64_t total_len = len + page_offset;
    uint64_t total_pages = PAGES_NEEDED_FOR(total_len);

    for (uint64_t i = 0; i < total_pages; i++) {
        vmm_unmap_page(aligned_virt + i * PAGE_SIZE, vflags);
    }
}

void *vmm_map_bump_internal(uintptr_t addr, uint64_t len, uint64_t flags,
                            enum vmm_flags vflags) {
    if (global.current_bootstage >= BOOTSTAGE_LATE)
        log_warn_once("vmm_map_bump called after BOOTSTAGE_LATE...");

    if (len == 0)
        return NULL;

    uintptr_t phys_start = PAGE_ALIGN_DOWN(addr);
    uintptr_t offset = addr - phys_start;

    uint64_t total_len = len + offset;
    uint64_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t span = total_pages * PAGE_SIZE;
    if (total_pages != 0 && span / PAGE_SIZE != total_pages)
        return NULL;
    if (vmm_map_top > VMM_MAP_LIMIT || span > VMM_MAP_LIMIT - vmm_map_top)
        return NULL;

    uintptr_t virt_start = vmm_map_top;
    vmm_map_top += span;

    enum errno e = ERR_OK;
    uint64_t mapped = 0;

    for (; mapped < total_pages; mapped++) {
        e = vmm_map_page(virt_start + mapped * PAGE_SIZE,
                         phys_start + mapped * PAGE_SIZE,
                         PAGE_PRESENT | PAGE_WRITE | flags, vflags);
        if (e < 0)
            goto unwind;
    }

    return (void *) (virt_start + offset);

unwind:
    for (uint64_t i = 0; i < mapped; i++)
        vmm_unmap_page(virt_start + i * PAGE_SIZE, vflags);

    vmm_map_top = virt_start;

    return NULL;
}

void vmm_unmap_virt(void *addr, uint64_t len, enum vmm_flags vflags) {
    uintptr_t virt_addr = (uintptr_t) addr;
    uintptr_t page_offset = virt_addr & (PAGE_SIZE - 1);
    uintptr_t aligned_virt = PAGE_ALIGN_DOWN(virt_addr);

    uint64_t total_len = len + page_offset;
    uint64_t total_pages = PAGES_NEEDED_FOR(total_len);

    for (uint64_t i = 0; i < total_pages; i++) {
        vmm_unmap_page(aligned_virt + i * PAGE_SIZE, vflags);
    }
}

struct page_table *vmm_phys_to_pml4(paddr_t paddr) {
    return (struct page_table *) hhdm_paddr_to_ptr(paddr);
}
