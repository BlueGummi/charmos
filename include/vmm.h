#include <printf.h>
#include <stdint.h>
#define PAGING_PRESENT (0x1L)
#define PAGING_WRITE (0x2L)
#define PAGING_USER_ALLOWED (0x4L)
#define PAGING_XD (1L << 63) // E(x)ecute (D)isable
#define PAGING_PHYS_MASK (0x00FFFFFFF000)
#define PAGING_PAGE_SIZE (1L << 7)
#define PAGING_UNCACHABLE (1L << 4)
#define PAGE_SIZE 4096
#define SUB_HHDM_OFFSET(v) (v - hhdm_offset)
#define PT_KERNEL_RO (PAGING_PRESENT | PAGING_XD)
#define PT_KERNEL_RX (PAGING_PRESENT)
#define PT_KERNEL_RW (PAGING_PRESENT | PAGING_WRITE | PAGING_XD)

/* This needs to fix paging and setup our custom page tables
 * rather than modify existing page tables (which may disappear) as they are in
 * bootloader-reclaimable memory. We need to create types to define page tables,
 * and then setup each table with incrementing addresses.
 *
 * I will attempt to make this as clear as I possibly can
 * (type aliases, and favoring duplicate types for clarity over reusing
 * uint64_t)
 */

typedef uint64_t PageTableEntry; // Page Table Entry

typedef struct {
    PageTableEntry entries[512];
} __attribute__((packed)) PageTable;

uint64_t sub_offset(uint64_t a);
void vmm_offset_set(uint64_t o);
unsigned long get_cr3(void);
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void vmm_init();
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void *vmm_map_region(uintptr_t virt_base, uint64_t size, uint64_t flags);
void vmm_unmap_page(uintptr_t virt);
extern PageTable *kernel_pml4;
