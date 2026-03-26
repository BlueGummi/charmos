#include <acpi/lapic.h>
#include <console/printf.h>
#include <global.h>
#include <irq/idt.h>
#include <limine.h>
#include <linker/symbols.h>
#include <mem/asan.h>
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
#define KERNEL_PML4_START_INDEX 256
#define ENTRY_PRESENT(entry) (entry & PAGE_PRESENT)

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

static struct pt_deferred_free *pt_free_list;
static struct spinlock pt_free_lock = SPINLOCK_INIT;
static struct page_table *kernel_pml4 = NULL;
static uintptr_t vmm_map_top = VMM_MAP_BASE;

static inline struct page_table *alloc_pt(void) {
    paddr_t phys = pmm_alloc_page();
    if (!phys)
        return NULL;

    void *virt = (void *) (phys + global.hhdm_offset);
    memset(virt, 0, PAGE_SIZE);
    return virt;
}

static enum errno pte_init(pte_t *entry, uint64_t flags) {
    struct page_table *new_table = alloc_pt();
    if (!new_table)
        return ERR_NO_MEM;

    uintptr_t new_table_phys = (uintptr_t) new_table - global.hhdm_offset;
    memset(new_table, 0, PAGE_SIZE);
    *entry = new_table_phys | PAGE_PRESENT | PAGE_WRITE | flags;
    return ERR_OK;
}

enum irql pte_lock(pte_t *pt) {
    enum pte_lock_result out;
    return pte_lock_irql((void *) pt, &out);
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
    return (struct page_table *) ((entry & PAGE_PHYS_MASK) +
                                  global.hhdm_offset);
}

static inline pte_t pt_make_table(paddr_t phys, uint64_t flags) {
    return phys | PAGE_PRESENT | PAGE_WRITE | flags;
}

static inline pte_t pt_make_page(paddr_t phys, uint64_t flags) {
    return (phys & PAGE_PHYS_MASK) | flags | PAGE_PRESENT;
}

static inline pte_t pt_make_2mb(paddr_t phys, uint64_t flags) {
    return (phys & PAGE_PHYS_MASK) | flags | PAGE_PRESENT | PAGE_2MB_page;
}

static bool pt_walk_to_level(struct pt_walk *w, uintptr_t virt,
                             int target_level, bool create) {
    w->tables[0] = kernel_pml4;

    for (int level = 0; level < target_level; level++) {
        uint64_t index = pt_index(virt, level);
        pte_t *entry = &w->tables[level]->entries[index];

        w->irqls[level] = pte_lock(entry);
        w->entries[level] = entry;

        if (!ENTRY_PRESENT(*entry)) {
            if (!create)
                return false;

            if (pte_init(entry, 0) < 0)
                return false;
        }

        w->tables[level + 1] = pt_next_table(*entry);
        w->depth = level + 1;
    }

    return true;
}

/* TODO: */
static void enqueue_pt_free(paddr_t phys) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return pmm_free_page(phys);

    struct pt_deferred_free *n = kmalloc(sizeof(*n));
    if (!n)
        panic("OOM freeing page table");

    n->phys = phys;
    n->epoch = atomic_load_explicit(&global.pt_epoch, memory_order_relaxed);

    enum irql irql = spin_lock(&pt_free_lock);
    n->next = pt_free_list;
    pt_free_list = n;
    spin_unlock(&pt_free_lock, irql);
}

void vmm_reclaim_page_tables(void) {
    kassert(irql_get() == IRQL_DISPATCH_LEVEL);
    /* recursion prevention */
    if (smp_core()->reclaiming_page_tables)
        return;

    smp_core()->reclaiming_page_tables = true;

    uint64_t min_epoch = UINT64_MAX;

    struct core *cpu;
    for_each_cpu_struct(cpu) {
        uint64_t seen = cpu->pt_seen_epoch;
        if (seen < min_epoch)
            min_epoch = seen;
    }

    enum irql irql = spin_lock(&pt_free_lock);

    struct pt_deferred_free **pp = &pt_free_list;
    while (*pp) {
        struct pt_deferred_free *n = *pp;

        if (n->epoch >= min_epoch) {
            pp = &n->next;
            continue;
        }

        *pp = n->next;
        spin_unlock(&pt_free_lock, irql);

        pmm_free_pages(n->phys, 1);
        kfree(n);

        irql = spin_lock(&pt_free_lock);
    }

    spin_unlock(&pt_free_lock, irql);

    smp_core()->reclaiming_page_tables = false;
}

uint64_t sub_offset(uint64_t a) {
    return a - global.hhdm_offset;
}

uintptr_t vmm_make_user_pml4(void) {
    struct page_table *user_pml4 = alloc_pt();
    if (!user_pml4) {
        panic("Failed to allocate user pml4");
    }
    memset(user_pml4, 0, PAGE_SIZE);

    for (int i = KERNEL_PML4_START_INDEX; i < PT_ENTRIES; i++) {
        user_pml4->entries[i] = kernel_pml4->entries[i];
    }

    return (uintptr_t) user_pml4 - global.hhdm_offset;
}

void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa) {
    kernel_pml4 = alloc_pt();
    if (!kernel_pml4)
        panic("Could not allocate space for kernel PML4\n");

    uintptr_t kernel_pml4_phys = (uintptr_t) kernel_pml4 - global.hhdm_offset;
    memset(kernel_pml4, 0, PAGE_SIZE);

    uint64_t kernel_phys_start = xa->physical_base;
    uint64_t kernel_virt_start = xa->virtual_base;
    uint64_t kernel_virt_end = (uint64_t) &__kernel_virt_end;
    uint64_t kernel_size = kernel_virt_end - kernel_virt_start;

    paddr_t dummy_phys = pmm_alloc_page();
    uint8_t *dummy_virt = (uint8_t *) (dummy_phys + global.hhdm_offset);
    memset(dummy_virt, 0xFF, PAGE_SIZE);

    enum errno e;

    for (uint64_t i = 0; i < kernel_size; i += PAGE_SIZE) {
        e = vmm_map_page(kernel_virt_start + i, kernel_phys_start + i,
                         PAGE_WRITE | PAGE_PRESENT, VMM_FLAG_NONE);
        if (e < 0)
            panic("Error %s whilst mapping kernel\n", errno_to_str(e));
    }

    for (uint64_t addr = kernel_virt_start; addr < kernel_virt_end;
         addr += PAGE_SIZE) {
        uint64_t shadow_addr = ASAN_SHADOW_OFFSET + (addr >> ASAN_SHADOW_SCALE);
        shadow_addr = PAGE_ALIGN_DOWN(shadow_addr);
        vmm_map_page(shadow_addr, dummy_phys, PAGE_PRESENT | PAGE_WRITE,
                     VMM_FLAG_NONE);
    }

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
        uint64_t flags = PAGE_PRESENT | PAGE_WRITE;

        if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            flags |= PAGE_WRITETHROUGH;
        }

        uint64_t phys = base;
        while (phys < end) {
            uint64_t virt = phys + global.hhdm_offset;

            bool can_use_2mb = ((phys % PAGE_2MB) == 0) &&
                               ((virt % PAGE_2MB) == 0) &&
                               ((end - phys) >= PAGE_2MB);

            if (can_use_2mb) {
                e = vmm_map_2mb_page(virt, phys, flags, VMM_FLAG_NONE);
                phys += PAGE_2MB;
            } else {
                e = vmm_map_page(virt, phys, flags, VMM_FLAG_NONE);
                phys += PAGE_SIZE;
            }
            if (e < 0)
                panic("Error %s whilst mapping kernel\n", errno_to_str(e));
        }
    }

    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

static inline bool vmm_is_table_empty(struct page_table *table) {
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (ENTRY_PRESENT(table->entries[i]))
            return false;
    }
    return true;
}

enum errno vmm_map_2mb_page(uintptr_t virt, uintptr_t phys, uint64_t flags,
                            enum vmm_flags vflags) {
    if (virt == 0 || !IS_ALIGNED(virt, PAGE_2MB) || !IS_ALIGNED(phys, PAGE_2MB))
        panic(
            "vmm_map_2mb_page: addresses must be 2MiB aligned and non-zero\n");

    struct page_table *tables[3];
    enum irql irqls[2];
    pte_t *ptes[2];

    tables[0] = kernel_pml4;

    enum errno ret = ERR_OK;
    int level = 0;
    for (level = 0; level < 2; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        pte_t *entry = &tables[level]->entries[index];
        irqls[level] = pte_lock(entry);
        ptes[level] = entry;

        if (!ENTRY_PRESENT(*entry))
            if ((ret = pte_init(entry, 0)) < 0)
                goto out;

        tables[level + 1] = (struct page_table *) ((*entry & PAGE_PHYS_MASK) +
                                                   global.hhdm_offset);
    }

    uint64_t L2 = (virt >> 21) & 0x1FF;
    pte_t *last_entry = &tables[2]->entries[L2];

    enum irql last_irql = pte_lock(last_entry);
    if (ENTRY_PRESENT(*last_entry))
        barrier_and_shootdown(vflags, virt);

    *last_entry =
        (phys & PAGE_PHYS_MASK) | flags | PAGE_PRESENT | PAGE_2MB_page;

    pte_unlock(last_entry, last_irql);

out:
    for (int i = level; i >= 0; i--)
        pte_unlock(ptes[i], irqls[i]);

    return ret;
}

void vmm_unmap_2mb_page(uintptr_t virt, enum vmm_flags vflags) {
    if (virt & (PAGE_2MB - 1))
        panic("vmm_unmap_2mb_page: virtual address not 2MiB aligned!\n");

    struct page_table *tables[3];
    pte_t *entries[2];
    enum irql irqls[2];

    tables[0] = kernel_pml4;

    int level = 0;
    for (level = 0; level < 2; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        pte_t *entry = &tables[level]->entries[index];
        irqls[level] = pte_lock(entry);
        entries[level] = entry;

        if (!ENTRY_PRESENT(*entries[level]))
            goto out;

        tables[level + 1] =
            (struct page_table *) ((*entries[level] & PAGE_PHYS_MASK) +
                                   global.hhdm_offset);
    }

    uint64_t L2 = (virt >> 21) & 0x1FF;

    pte_t *last_entry = &tables[2]->entries[L2];

    enum irql last_irql = pte_lock(last_entry);

    *last_entry &= ~PAGE_PRESENT;

    barrier_and_shootdown(vflags, virt);

    pte_unlock(last_entry, last_irql);

    for (int level_inner = 2; level_inner > 0; level_inner--) {
        if (vmm_is_table_empty(tables[level_inner])) {
            uintptr_t phys =
                (uintptr_t) tables[level_inner] - global.hhdm_offset;
            *entries[level_inner - 1] = 0;
            enqueue_pt_free(phys);
        } else {
            break;
        }
    }

out:
    for (int i = level - 1; i >= 0; i--)
        pte_unlock(entries[i], irqls[i]);
}

enum errno vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags,
                        enum vmm_flags vflags) {
    if (virt == 0)
        panic("CANNOT MAP PAGE 0x0!!!\n");

    enum errno ret = ERR_OK;
    struct page_table *tables[4]; // PML4, PDPT, PD, PT
    enum irql irqls[3];
    pte_t *entries[3];

    tables[0] = kernel_pml4;

    int level = 0;
    for (level = 0; level < 3; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        pte_t *entry = &tables[level]->entries[index];
        entries[level] = entry;
        irqls[level] = pte_lock(entry);

        if (!ENTRY_PRESENT(*entry))
            if ((ret = pte_init(entry, 0)) < 0)
                goto out;

        tables[level + 1] = (struct page_table *) ((*entry & PAGE_PHYS_MASK) +
                                                   global.hhdm_offset);
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    pte_t *last_entry = &tables[3]->entries[L1];

    enum irql last_irql = pte_lock(last_entry);

    if (ENTRY_PRESENT(*last_entry))
        barrier_and_shootdown(vflags, virt);

    *last_entry = (phys & PAGE_PHYS_MASK) | flags | PAGE_PRESENT;

    pte_unlock(last_entry, last_irql);

out:
    for (int i = level - 1; i >= 0; i--)
        pte_unlock(entries[i], irqls[i]);

    return ret;
}

void vmm_unmap_page(uintptr_t virt, enum vmm_flags vflags) {
    struct page_table *tables[4];
    pte_t *entries[3];
    enum irql irqls[3];

    tables[0] = kernel_pml4;
    int level = 0;
    for (level = 0; level < 3; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        pte_t *entry = &tables[level]->entries[index];

        irqls[level] = pte_lock(entry);
        entries[level] = entry;

        if (!ENTRY_PRESENT(*entries[level]))
            goto out;

        tables[level + 1] =
            (struct page_table *) ((*entries[level] & PAGE_PHYS_MASK) +
                                   global.hhdm_offset);
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;

    pte_t *last_entry = &tables[3]->entries[L1];

    enum irql last_irql = pte_lock(last_entry);

    *last_entry &= ~PAGE_PRESENT;

    barrier_and_shootdown(vflags, virt);

    pte_unlock(last_entry, last_irql);

    for (int level_inner = 3; level_inner > 0; level_inner--) {
        if (vmm_is_table_empty(tables[level_inner])) {
            uintptr_t phys =
                (uintptr_t) tables[level_inner] - global.hhdm_offset;
            *entries[level_inner - 1] = 0;
            enqueue_pt_free(phys);
        } else {
            break;
        }
    }

out:
    for (int i = level - 1; i >= 0; i--)
        pte_unlock(entries[i], irqls[i]);
}

uintptr_t vmm_get_phys(uintptr_t virt, enum vmm_flags vflags) {
    (void) vflags;
    struct page_table *tables[4] = {0};
    enum irql irqls[3] = {0};
    pte_t *entries[3] = {0};
    pte_t *entry = NULL;
    uintptr_t phys = (uintptr_t) -1;

    tables[0] = kernel_pml4;

    for (int level = 0; level < 3; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        entry = &tables[level]->entries[index];

        irqls[level] = pte_lock(entry);
        entries[level] = entry;

        if (!ENTRY_PRESENT(*entry)) {
            goto cleanup;
        }

        if (level == 2 && (*entry & PAGE_2MB_page)) {
            uintptr_t phys_base = *entry & PAGE_2MB_PHYS_MASK;
            uintptr_t offset = virt & (PAGE_2MB - 1);
            phys = phys_base + offset;
            goto cleanup;
        }

        tables[level + 1] = (struct page_table *) ((*entry & PAGE_PHYS_MASK) +
                                                   global.hhdm_offset);
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    entry = &tables[3]->entries[L1];
    enum irql last_irql = pte_lock(entry);

    if (!ENTRY_PRESENT(*entry)) {
        pte_unlock(entry, last_irql);
        goto cleanup;
    }

    uintptr_t phys_base = *entry & PAGE_PHYS_MASK;
    uintptr_t offset = virt & 0xFFF;
    phys = phys_base + offset;
    pte_unlock(entry, last_irql);

cleanup:
    for (int i = 2; i >= 0; i--) {
        if (entries[i])
            pte_unlock(entries[i], irqls[i]);
    }

    return phys;
}

uintptr_t vmm_get_phys_unsafe(uintptr_t virt) {
    struct page_table *current_table = kernel_pml4;

    for (uint64_t i = 0; i < 2; i++) {
        uint64_t level = (virt >> (39 - i * 9)) & 0x1FF;
        pte_t *entry = &current_table->entries[level];
        if (!ENTRY_PRESENT(*entry))
            goto err;

        current_table = (struct page_table *) ((*entry & PAGE_PHYS_MASK) +
                                               global.hhdm_offset);
    }

    uint64_t L2 = (virt >> 21) & 0x1FF;
    pte_t *entry = &current_table->entries[L2];

    if (!ENTRY_PRESENT(*entry))
        goto err;

    if (*entry & PAGE_2MB_page) {
        uintptr_t phys_base = *entry & PAGE_2MB_PHYS_MASK;
        uintptr_t offset = virt & (PAGE_2MB - 1);
        return phys_base + offset;
    }

    current_table =
        (struct page_table *) ((*entry & PAGE_PHYS_MASK) + global.hhdm_offset);

    uint64_t L1 = (virt >> 12) & 0x1FF;
    entry = &current_table->entries[L1];

    if (!ENTRY_PRESENT(*entry))
        goto err;

    return (*entry & PAGE_PHYS_MASK) + (virt & 0xFFF);

err:
    return (uintptr_t) -1;
}

void *vmm_map_phys(uint64_t addr, uint64_t len, uint64_t flags,
                   enum vmm_flags vflags) {

    uintptr_t phys_start = PAGE_ALIGN_DOWN(addr);
    uintptr_t offset = addr - phys_start;

    uint64_t total_len = len + offset;
    uint64_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    if (vmm_map_top + total_pages * PAGE_SIZE > VMM_MAP_LIMIT) {
        return NULL;
    }

    uintptr_t virt_start = vmm_map_top;
    vmm_map_top += total_pages * PAGE_SIZE;

    for (uint64_t i = 0; i < total_pages; i++) {
        vmm_map_page(virt_start + i * PAGE_SIZE, phys_start + i * PAGE_SIZE,
                     PAGE_PRESENT | PAGE_WRITE | flags, vflags);
    }

    return (void *) (virt_start + offset);
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
