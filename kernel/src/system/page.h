#include <stdint.h>
unsigned long get_cr3(void);
uint64_t add_offset(uint64_t cr3);
void init_paging(uint64_t offset);
void paging_map_cr3(uint64_t phys, uint64_t virt, uint64_t permissions);
void paging_unmap_cr3(uint64_t virt);
#define PAGING_X86_64_PRESENT (0x1L)
#define PAGING_X86_64_WRITE (0x2L)
#define PAGING_X86_64_USER_ALLOWED (0x4L)
#define PAGING_X86_64_EXECUTE_DISABLE (1L << 63)
#define PAGING_X86_64_PHYS_MASK (0x00FFFFFFF000)
#define PAGING_X86_64_PS (1L << 7)
#define PAGING_X86_64_UNCACHABLE (1L << 4)
#define PAGE_SIZE 4096
#define CONVERT_ADDR(v) (v + o)
#define LOCAL_OFFSET local_offset
