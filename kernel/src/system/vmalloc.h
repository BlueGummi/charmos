#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
extern uint64_t hhdm_offset;
void vmm_bitmap_init(uintptr_t base_address, size_t total_pages);
void *vmm_alloc_pages(size_t count);
void *vmm_alloc_page();
void vmm_free_pages(void *address, size_t count);
size_t vmm_get_free_pages(void);
size_t vmm_get_total_pages(void);
#define BITMAP_SIZE (1 << 20) // four giggybites
