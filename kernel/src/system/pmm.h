#include <limine.h>
#include <stddef.h>

void init_physical_allocator(uint64_t o, struct limine_memmap_request m);
void *pmm_alloc_page();
void *allocate_page();
void free_page(void *addr);
void *pmm_alloc_pages(size_t count);
void pmm_free_pages(void *addr, size_t count);
