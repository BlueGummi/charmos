/* @title: Domain buddy allocator */
#include <mem/alloc.h>
#include <smp/domain.h>
#include <stdint.h>
#include <types/types.h>

void domain_buddies_init(void);
void domain_free(paddr_t address, size_t page_count);
paddr_t domain_alloc(size_t pages, enum alloc_flags flags);
paddr_t domain_alloc_from_domain(struct domain *cd, size_t pages);
void domain_buddy_dump(void);
struct domain *domain_for_addr(paddr_t addr);
