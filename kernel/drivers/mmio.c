#include <drivers/mmio.h>
#include <mem/address_range.h>
#include <mem/nofail.h>
#include <mem/page.h>
#include <mem/vas.h>
#include <mem/vmm.h>

static struct vas *mmio_vas_space = NULL;
ADDRESS_RANGE_DEFINE(mmio, .align = PAGE_1GB, .flags = ADDRESS_RANGE_DYNAMIC,
                     .size = MMIO_RANGE_SIZE);

void mmio_init() {
    mmio_vas_space = nofail(vas_from(&ADDRESS_RANGE(mmio)));
}

static inline cc_always_inline void *mmio_map_raw(paddr_t phys, size_t size,
                                                  bool cacheable) {
    kassert(mmio_vas_space);
    return vas_map(mmio_vas_space, phys, size,
                   .page_flags =
                       PAGE_WRITE | (cacheable ? PAGE_UNCACHABLE : 0));
}

void cc_mem_io *mmio_map(paddr_t phys, size_t size) {
    return (void cc_mem_io *) mmio_map_raw(phys, size, false);
}

void *mmio_map_dma(paddr_t phys, size_t size) {
    return mmio_map_raw(phys, size, true);
}

void mmio_unmap(void cc_mem_io *vaddr, size_t size) {
    kassert(mmio_vas_space);
    vas_unmap(mmio_vas_space, (void *) vaddr, size);
}
