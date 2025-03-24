#include <stdint.h>
#include <system/memfuncs.h>
#include <system/pmm.h>
#include <system/printf.h>
#include <system/vmm.h>
#define PAGING_PRESENT (0x1L)
#define PAGING_WRITE (0x2L)
#define PAGING_USER_ALLOWED (0x4L)
#define PAGING_XD (1L << 63) // E(x)ecute (D)isable
#define PAGING_PHYS_MASK (0x00FFFFFFF000)
#define PAGING_PAGE_SIZE (1L << 7)
#define PAGING_UNCACHABLE (1L << 4)
#define PAGE_SIZE 4096
#define SUB_HHDM_OFFSET(v) (v - hhdm_offset)

PageTable *kernel_pml4 = NULL;
uintptr_t kernel_pml4_phys = 0;

uint64_t hhdm_offset = 0;
uint64_t sub_offset(uint64_t a) {
    return a - hhdm_offset;
}
void vmm_offset_set(uint64_t o) { 
    hhdm_offset = o;
}
unsigned long get_cr3(void) {
    unsigned long cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

VirtAddr vmm_extract_virtaddr(uint64_t address) {
    VirtAddr vaddr = {
        .L4 = (address >> 39) & 0x1FF,
        .L3 = (address >> 30) & 0x1FF,
        .L2 = (address >> 21) & 0x1FF,
        .L1 = (address >> 12) & 0x1FF,
        .offset = address & 0xFFF,
    };
    return vaddr;
}

void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void vmm_copy_kernel_mappings(uintptr_t new_virt_base) {
    uintptr_t old_virt_start = 0xffffffff80000000;
    uintptr_t old_virt_end = 0xffffffff80102000;

    for (uintptr_t old_virt = old_virt_start; old_virt < old_virt_end; old_virt += PAGE_SIZE) {
        uintptr_t phys = SUB_HHDM_OFFSET(old_virt);
        if (phys == (uintptr_t) -1)
            continue;

        uintptr_t new_virt = new_virt_base + (old_virt - old_virt_start);

        uint64_t flags = PAGING_PRESENT;
        if (old_virt >= 0xffffffff80001000 && old_virt < 0xffffffff8000b000) {
            flags |= PAGING_WRITE;
        }

        vmm_map_page(new_virt, phys, flags);
    }
}
void debug_mappings(uintptr_t virt_start, uintptr_t virt_end) {
    for (uintptr_t virt = virt_start; virt < virt_end; virt += PAGE_SIZE) {
        uintptr_t phys = SUB_HHDM_OFFSET(virt);
        if (phys != (uintptr_t) -1) {
            k_printf("0x%llx -> 0x%llx\n", virt, phys);
        }
    }
}
void vmm_init() {
    kernel_pml4 = (PageTable *) pmm_alloc_page();
    kernel_pml4_phys = (uintptr_t) kernel_pml4 - hhdm_offset;
    memset(kernel_pml4, 0, PAGE_SIZE);

    uintptr_t boot_cr3 = get_cr3();
    PageTable *boot_pml4 = (PageTable *) (boot_cr3 + hhdm_offset);
    for (size_t i = 0; i < 512; i++) {
        if (boot_pml4->entries[i] & PAGING_PRESENT) {
            kernel_pml4->entries[i] = boot_pml4->entries[i];
        }
    }
    debug_mappings(0xffffffff80000000, 0xffffffff80102000);
    debug_mappings(0xffffffffc0000000, 0xffffffffc0102000);
    vmm_copy_kernel_mappings(0xffffffffc0000000);
    k_printf("Yo yo yo we aboutta switch cr3 please check info mem\n");
    asm volatile("hlt");
    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
    k_printf("FAM! we just reloaded CR3, with address 0x%zx\nVirt addr 0x%zx", kernel_pml4_phys, kernel_pml4);
}

void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    VirtAddr vaddr = vmm_extract_virtaddr(virt);

    PageTable *current_table = kernel_pml4;

    PageTableEntry *entry = &current_table->entries[vaddr.L4];
    if (!(*entry & PAGING_PRESENT)) {
        PageTable *new_table = (PageTable *) pmm_alloc_page();
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[vaddr.L3];
    if (!(*entry & PAGING_PRESENT)) {
        PageTable *new_table = (PageTable *) pmm_alloc_page();
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[vaddr.L2];
    if (!(*entry & PAGING_PRESENT)) {
        PageTable *new_table = (PageTable *) pmm_alloc_page();
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[vaddr.L1];
    *entry = (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT;

    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_unmap_page(uintptr_t virt) {
    VirtAddr vaddr = vmm_extract_virtaddr(virt);
    PageTable *current_table = kernel_pml4;

    PageTableEntry *entry = &current_table->entries[vaddr.L4];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[vaddr.L3];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[vaddr.L2];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[vaddr.L1];
    *entry &= ~PAGING_PRESENT;

    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
