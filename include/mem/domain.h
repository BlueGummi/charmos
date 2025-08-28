#include <mem/alloc.h>
#include <stdint.h>
#include <types/types.h>

void domain_buddies_init(void);
void domain_free(paddr_t address, size_t page_count);
paddr_t domain_alloc(size_t pages, enum alloc_class class,
                     enum alloc_flags flags);
void domain_dump(void);
