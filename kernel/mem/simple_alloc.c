#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vaddr_alloc.h>
#include <mem/vmm.h>
#include <string.h>
/* simple alloc used for bootstrapping systems */
void *simple_alloc(struct vas_space *space, size_t size) {
    size_t pages = PAGES_NEEDED_FOR(size);
    paddr_t phys_base = pmm_alloc_pages(pages, ALLOC_FLAGS_DEFAULT);

    vaddr_t area = vas_alloc(space, size, PAGE_SIZE);

    for (size_t i = 0; i < pages; i++) {
        vaddr_t virt = area + (i * PAGE_SIZE);
        paddr_t phys = phys_base + (i * PAGE_SIZE);
        if (vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE,
                         VMM_FLAG_NONE) < 0)
            k_panic("Could not do simple_alloc!\n");
    }

    memset((void *) area, 0, size);
    return (void *) area;
}
