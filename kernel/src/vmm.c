#include <stdint.h>
#include <system/memfuncs.h>
#include <system/pmm.h>
#include <system/printf.h>
#include <system/vmalloc.h>
#include <system/vmm.h>

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

void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);

void vmm_copy_kernel_mappings(uintptr_t new_virt_base) {
    extern uint64_t __stext[], __etext[];
    uintptr_t old_virt_start = (uintptr_t) __stext;
    uintptr_t old_virt_end = (uintptr_t) __etext;

    for (uintptr_t old_virt = old_virt_start; old_virt < old_virt_end; old_virt += PAGE_SIZE) {
        uintptr_t phys = SUB_HHDM_OFFSET(old_virt);
        if (phys == (uintptr_t) -1)
            continue;

        uintptr_t new_virt = new_virt_base + (old_virt - old_virt_start);

        uint64_t flags = PAGING_PRESENT;
        if (old_virt >= (uintptr_t) (__stext + 0x1000) &&
            old_virt < (uintptr_t) (__stext + 0xb000)) {
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

void map_custom_region(uintptr_t virt_base, size_t size, uint64_t flags, const char *name) {
    k_info("Mapping custom region \"%s\" of size 0x%zx, at base v.addr of 0x%zx", name, size, virt_base);
    for (uintptr_t virt = virt_base; virt < virt_base + size; virt += PAGE_SIZE) {
        uintptr_t phys = (uintptr_t) pmm_alloc_page() - hhdm_offset;
        if (phys == (uintptr_t) -1) {
            k_panic("Error: Out of memory mapping %s\n", name);
            return;
        }
        vmm_map_page(virt, phys, flags);
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

    extern uint64_t __stext[], __etext[];
    extern uint64_t __srodata[], __erodata[];
    extern uint64_t __sdata[], __edata[];
    extern uint64_t __sbss[], __ebss[];
    extern uint64_t __slimine_requests[], __elimine_requests[];

    k_info("Kernel sections:\n");
    k_printf("  text:   0x%zx - 0x%zx\n", __stext, __etext);
    k_printf("  rodata: 0x%zx - 0x%zx\n", __srodata, __erodata);
    k_printf("  data:   0x%zx - 0x%zx\n", __sdata, __edata);
    k_printf("  bss:    0x%zx - 0x%zx\n", __sbss, __ebss);
    k_printf("  limine: 0x%zx - 0x%zx\n", __slimine_requests, __elimine_requests);

    for (uintptr_t virt = (uintptr_t) __slimine_requests;
         virt < (uintptr_t) __elimine_requests;
         virt += PAGE_SIZE) {
        uintptr_t phys = (uintptr_t) pmm_alloc_page() - hhdm_offset;
        vmm_map_page(virt, phys, PT_KERNEL_RW);
    }

    map_custom_region(0xffffffff00000000, 0x100000, PT_KERNEL_RW, "Debug area");
    map_custom_region(0xffffffff10000000, 0x40000, PT_KERNEL_RO, "Console buffer");
    map_custom_region(0xffffffff20000000, 0x2000000, PT_KERNEL_RW | PAGING_UNCACHABLE, "Device memory");
    map_custom_region(0xffff800000000000, BITMAP_SIZE, PT_KERNEL_RW, "VMM Allocator");
    map_custom_region((uintptr_t) __stext, (uint64_t) __etext - (uint64_t) __stext, PT_KERNEL_RO, "Text segment");
    map_custom_region((uintptr_t) __sdata, (uint64_t) __edata - (uint64_t) __sdata, PT_KERNEL_RO, "Data segment");
    vmm_copy_kernel_mappings(0xffffffffc0000000);

    vmm_bitmap_init(0xffff800000000000, 0x100000);
    k_info("My value is 0x%zu", kernel_pml4);
    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
    k_info("My value is 0x%zu", kernel_pml4);
}

/* This is our virtual memory paging system. We get a virtual address, and a physical address to map it to.
 *
 * We can map a virtual address from anywhere we'd like, but physical addresses must be retrieved elsewhere.
 *
 * Offsets can be subtracted from addresses allocated by pmm_alloc_page
 */

/* TODO
 *
 * we can avoid the multiple page allocs by allocating one and doing offsets 
 */
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    PageTable *current_table = kernel_pml4;

    PageTableEntry *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT)) {
        PageTable *new_table = (PageTable *) pmm_alloc_page();
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT)) {
        PageTable *new_table = (PageTable *) pmm_alloc_page();
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT)) {
        PageTable *new_table = (PageTable *) pmm_alloc_page();
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    *entry = (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT;

    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_unmap_page(uintptr_t virt) {
    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;
    PageTable *current_table = kernel_pml4;
    PageTableEntry *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    *entry &= ~PAGING_PRESENT;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void *vmm_alloc_page() {
    return vmm_alloc_pages(1);
}
