#include <acpi/lapic.h>
#include <console/printf.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/linker_symbols.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>
#define KERNEL_PML4_START_INDEX 256
#define ENTRY_PRESENT(entry) (entry & PAGING_PRESENT)

static struct spinlock vmm_lock = SPINLOCK_INIT;
static struct page_table *kernel_pml4 = NULL;
static uintptr_t kernel_pml4_phys = 0;
static uintptr_t vmm_map_top = VMM_MAP_BASE;

uint64_t sub_offset(uint64_t a) {
    return a - global.hhdm_offset;
}

static inline struct page_table *alloc_pt(void) {
    struct page_table *ret =
        (void *) (pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE) +
                  global.hhdm_offset);
    if (!ret)
        return NULL;

    memset(ret, 0, PAGE_SIZE);
    return ret;
}

uintptr_t vmm_make_user_pml4(void) {
    struct page_table *user_pml4 = alloc_pt();
    if (!user_pml4) {
        k_panic("Failed to allocate user pml4");
    }
    memset(user_pml4, 0, PAGE_SIZE);

    for (int i = KERNEL_PML4_START_INDEX; i < 512; i++) {
        user_pml4->entries[i] = kernel_pml4->entries[i];
    }

    return (uintptr_t) user_pml4 - global.hhdm_offset;
}

void tlb_shootdown(void *ctx, uint8_t irq, void *rsp) {
    (void) ctx, (void) irq, (void) rsp;

    struct core *core = global.cores[get_this_core_id()];
    uint64_t req_gen =
        atomic_load_explicit(&core->tlb_req_gen, memory_order_acquire);

    uintptr_t page =
        atomic_load_explicit(&core->tlb_page, memory_order_acquire);
    if (page)
        invlpg(page);

    atomic_store_explicit(&core->tlb_page, 0, memory_order_release);
    atomic_store_explicit(&core->tlb_ack_gen, req_gen, memory_order_release);

    lapic_write(LAPIC_REG_EOI, 0);
}

static void do_tlb_shootdown(uintptr_t addr) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return;

    uint64_t gen = atomic_fetch_add(&global.next_tlb_gen, 1);
    uint64_t this_core = get_this_core_id();
    uint64_t cores = global.core_count;

    for (uint64_t i = 0; i < cores; i++) {
        if (i == this_core)
            continue;

        struct core *target = global.cores[i];
        atomic_store_explicit(&target->tlb_page, addr, memory_order_release);
        atomic_store_explicit(&target->tlb_req_gen, gen, memory_order_release);
        ipi_send(i, IRQ_TLB_SHOOTDOWN);
    }

    for (uint64_t i = 0; i < cores; i++) {
        if (i == this_core)
            continue;

        struct core *target = global.cores[i];

        while (atomic_load_explicit(&target->tlb_ack_gen,
                                    memory_order_acquire) < gen)
            cpu_relax();
    }
}

void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa) {
    kernel_pml4 = alloc_pt();
    if (!kernel_pml4)
        k_panic("Could not allocate space for kernel PML4\n");

    kernel_pml4_phys = (uintptr_t) kernel_pml4 - global.hhdm_offset;
    memset(kernel_pml4, 0, PAGE_SIZE);

    uint64_t kernel_phys_start = xa->physical_base;
    uint64_t kernel_virt_start = xa->virtual_base;
    uint64_t kernel_virt_end = (uint64_t) &__kernel_virt_end;
    uint64_t kernel_size = kernel_virt_end - kernel_virt_start;

    enum errno e;

    for (uint64_t i = 0; i < kernel_size; i += PAGE_SIZE) {
        e = vmm_map_page(kernel_virt_start + i, kernel_phys_start + i,
                         PAGING_WRITE | PAGING_PRESENT);
        if (e < 0)
            k_panic("Error %s whilst mapping kernel\n", errno_to_str(e));
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
        uint64_t flags = PAGING_PRESENT | PAGING_WRITE;

        if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            flags |= PAGING_WRITETHROUGH;
        }

        uint64_t phys = base;
        while (phys < end) {
            uint64_t virt = phys + global.hhdm_offset;

            bool can_use_2mb = ((phys % PAGE_2MB) == 0) &&
                               ((virt % PAGE_2MB) == 0) &&
                               ((end - phys) >= PAGE_2MB);

            if (can_use_2mb) {
                e = vmm_map_2mb_page(virt, phys, flags);
                phys += PAGE_2MB;
            } else {
                e = vmm_map_page(virt, phys, flags);
                phys += PAGE_SIZE;
            }
            if (e < 0)
                k_panic("Error %s whilst mapping kernel\n", errno_to_str(e));
        }
    }

    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

static inline bool vmm_is_table_empty(struct page_table *table) {
    for (int i = 0; i < 512; i++) {
        if (table->entries[i] & PAGING_PRESENT)
            return false;
    }
    return true;
}

static enum errno pte_init(pte_t *entry, uint64_t flags) {
    struct page_table *new_table = alloc_pt();
    if (!new_table)
        return ERR_NO_MEM;

    uintptr_t new_table_phys = (uintptr_t) new_table - global.hhdm_offset;
    memset(new_table, 0, PAGE_SIZE);
    *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE | flags;
    return ERR_OK;
}

enum errno vmm_map_2mb_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    if (virt == 0 || (virt & 0x1FFFFF) || (phys & 0x1FFFFF)) {
        k_panic("vmm_map_2mb_page: addresses must be 2MiB aligned and "
                "non-zero, virt=0x%lx phys=0x%lx\n",
                virt, phys);
    }

    enum irql irql = spin_lock_irq_disable(&vmm_lock);
    uint64_t L2 = (virt >> 21) & 0x1FF;

    struct page_table *current_table = kernel_pml4;
    for (uint64_t i = 0; i < 2; i++) {
        uint64_t level = (virt >> (39 - (i * 9))) & 0x1FF;
        pte_t *entry = &current_table->entries[level];

        if (!ENTRY_PRESENT(*entry)) {
            enum errno ret = pte_init(entry, 0);
            if (ret < 0) {
                spin_unlock(&vmm_lock, irql);
                return ret;
            }
        }
        current_table = (struct page_table *) ((*entry & PAGING_PHYS_MASK) +
                                               global.hhdm_offset);
    }

    pte_t *entry = &current_table->entries[L2];
    if (ENTRY_PRESENT(*entry)) {
        invlpg(virt);
        do_tlb_shootdown(virt);
    }
    *entry =
        (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT | PAGING_2MB_page;

    spin_unlock(&vmm_lock, irql);
    return 0;
}

void vmm_unmap_2mb_page(uintptr_t virt) {
    if ((virt & (PAGE_2MB - 1)) != 0)
        k_panic("vmm_unmap_2mb_page: virtual address not 2MiB aligned!\n");

    struct page_table *tables[3];
    pte_t *entries[3];

    enum irql irql = spin_lock_irq_disable(&vmm_lock);
    tables[0] = kernel_pml4;

    for (uint64_t level = 0; level < 2; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        entries[level] = &tables[level]->entries[index];

        if (!ENTRY_PRESENT(*entries[level])) {
            spin_unlock(&vmm_lock, irql);
            return;
        }

        tables[level + 1] =
            (struct page_table *) ((*entries[level] & PAGING_PHYS_MASK) +
                                   global.hhdm_offset);
    }

    uint64_t L2 = (virt >> 21) & 0x1FF;
    entries[2] = &tables[2]->entries[L2];

    *entries[2] &= ~PAGING_PRESENT;
    invlpg(virt);
    do_tlb_shootdown(virt);

    for (uint64_t level = 2; level > 0; level--) {
        if (vmm_is_table_empty(tables[level])) {
            paddr_t phys = (uintptr_t) tables[level] - global.hhdm_offset;
            *entries[level - 1] = 0;
            pmm_free_pages(phys, 1);
        } else {
            break;
        }
    }

    spin_unlock(&vmm_lock, irql);
}

enum errno vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    if (virt == 0) {
        k_panic("CANNOT MAP PAGE 0x0!!!\n");
    }

    enum irql irql = spin_lock_irq_disable(&vmm_lock);
    struct page_table *current_table = kernel_pml4;

    for (uint64_t i = 0; i < 3; i++) {
        uint64_t level = (virt >> (39 - (i * 9))) & 0x1FF;
        pte_t *entry = &current_table->entries[level];

        if (!ENTRY_PRESENT(*entry)) {
            enum errno ret = pte_init(entry, 0);
            if (ret < 0) {
                spin_unlock(&vmm_lock, irql);
                return ret;
            }
        }
        current_table = (struct page_table *) ((*entry & PAGING_PHYS_MASK) +
                                               global.hhdm_offset);
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    pte_t *entry = &current_table->entries[L1];
    if (ENTRY_PRESENT(*entry)) {
        invlpg(virt);
        do_tlb_shootdown(virt);
    }

    *entry = (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT;

    spin_unlock(&vmm_lock, irql);
    return 0;
}

void vmm_map_page_user(uintptr_t pml4_phys, uintptr_t virt, uintptr_t phys,
                       uint64_t flags) {
    if (virt == 0) {
        k_panic("CANNOT MAP PAGE 0x0!!!\n");
    }

    struct page_table *current_table =
        (struct page_table *) (pml4_phys + global.hhdm_offset);

    for (uint64_t i = 0; i < 3; i++) {
        uint64_t level = virt >> (39 - (i * 9)) & 0x1FF;
        pte_t *entry = &current_table->entries[level];
        if (!ENTRY_PRESENT(*entry))
            pte_init(entry, PAGING_USER_ALLOWED);

        current_table = (struct page_table *) ((*entry & PAGING_PHYS_MASK) +
                                               global.hhdm_offset);
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    pte_t *entry = &current_table->entries[L1];
    *entry = (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT;
}

void vmm_unmap_page(uintptr_t virt) {
    struct page_table *tables[4];
    pte_t *entries[4];

    enum irql irql = spin_lock_irq_disable(&vmm_lock);

    tables[0] = kernel_pml4;
    for (uint64_t level = 0; level < 3; level++) {
        uint64_t index = (virt >> (39 - level * 9)) & 0x1FF;
        entries[level] = &tables[level]->entries[index];

        if (!(*entries[level] & PAGING_PRESENT)) {
            spin_unlock(&vmm_lock, irql);
            return;
        }

        tables[level + 1] =
            (struct page_table *) ((*entries[level] & PAGING_PHYS_MASK) +
                                   global.hhdm_offset);
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    entries[3] = &tables[3]->entries[L1];

    *entries[3] &= ~PAGING_PRESENT;
    invlpg(virt);
    do_tlb_shootdown(virt);
    for (uint64_t level = 3; level > 0; level--) {
        if (vmm_is_table_empty(tables[level])) {
            uintptr_t phys = (uintptr_t) tables[level] - global.hhdm_offset;
            *entries[level - 1] = 0;
            pmm_free_pages(phys, 1);
        } else {
            break;
        }
    }
    spin_unlock(&vmm_lock, irql);
}

uintptr_t vmm_get_phys(uintptr_t virt) {
    struct page_table *current_table = kernel_pml4;

    for (uint64_t i = 0; i < 2; i++) {
        uint64_t level = (virt >> (39 - i * 9)) & 0x1FF;
        pte_t *entry = &current_table->entries[level];

        if (!ENTRY_PRESENT(*entry))
            return (uintptr_t) -1;

        current_table = (struct page_table *) ((*entry & PAGING_PHYS_MASK) +
                                               global.hhdm_offset);
    }

    uint64_t L2 = (virt >> 21) & 0x1FF;
    pte_t *entry = &current_table->entries[L2];

    if (!ENTRY_PRESENT(*entry))
        return (uintptr_t) -1;

    if (*entry & PAGING_2MB_page) {
        uintptr_t phys_base = *entry & PAGING_2MB_PHYS_MASK;
        uintptr_t offset = virt & (PAGE_2MB - 1);
        return phys_base + offset;
    }

    current_table = (struct page_table *) ((*entry & PAGING_PHYS_MASK) +
                                           global.hhdm_offset);
    uint64_t L1 = (virt >> 12) & 0x1FF;
    entry = &current_table->entries[L1];

    if (!ENTRY_PRESENT(*entry))
        return (uintptr_t) -1;

    return (*entry & PAGING_PHYS_MASK) + (virt & 0xFFF);
}

void *vmm_map_phys(uint64_t addr, uint64_t len, uint64_t flags) {

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
                     PAGING_PRESENT | PAGING_WRITE | flags);
    }

    return (void *) (virt_start + offset);
}

void vmm_unmap_virt(void *addr, uint64_t len) {
    uintptr_t virt_addr = (uintptr_t) addr;
    uintptr_t page_offset = virt_addr & (PAGE_SIZE - 1);
    uintptr_t aligned_virt = PAGE_ALIGN_DOWN(virt_addr);

    uint64_t total_len = len + page_offset;
    uint64_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < total_pages; i++) {
        vmm_unmap_page(aligned_virt + i * PAGE_SIZE);
    }
}
