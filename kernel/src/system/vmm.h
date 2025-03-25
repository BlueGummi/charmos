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

/* This needs to fix paging and setup our custom page tables
 * rather than modify existing page tables (which may disappear) as they are in bootloader-reclaimable memory.
 * We need to create types to define page tables, and then setup each table with incrementing addresses.
 *
 * I will attempt to make this as clear as I possibly can
 * (type aliases, and favoring duplicate types for clarity over reusing uint64_t)
 */

typedef uint64_t PageTableEntry; // Page Table Entry

typedef struct {
    PageTableEntry entries[512];
} __attribute__((packed)) PageTable;
typedef struct {
    union { // Using unions for name aliases
        uint16_t PageGlobalDirectory;
        uint16_t PGD;
        uint16_t L4;
    };
    union {
        uint16_t PageUpperDirectory;
        uint16_t PUD;
        uint16_t L3;
    };
    union {
        uint16_t PageMiddleDirectory;
        uint16_t PMD;
        uint16_t L2;
    };
    union {
        uint16_t PageTableEntry;
        uint16_t PTE;
        uint16_t L1;
    };
    uint16_t offset;
} VirtAddr;

uint64_t sub_offset(uint64_t a);
void vmm_offset_set(uint64_t o);
VirtAddr vmm_extract_virtaddr(uint64_t address);
unsigned long get_cr3(void);
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void vmm_init();
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void vmm_unmap_page(uintptr_t virt);
