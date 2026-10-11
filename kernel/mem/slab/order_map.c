#include <math/align.h>
#include <mem/must.h>
#include <mem/page.h>

#include "internal.h"

static atomic_uint8_t *order_map_entry_for(vaddr_t vaddr, bool *high_bits) {
    kassert(valloc_ptr_in_slab_validate((void *) vaddr));
    vaddr_t vptr_relative = vaddr - SLAB_HEAP_START;
    vaddr_t aligned_2mb = ALIGN_DOWN(vptr_relative, PAGE_2MB);
    vaddr_t aligned_4mb = ALIGN_DOWN(vptr_relative, PAGE_2MB * 2);
    *high_bits = aligned_2mb != aligned_4mb;
    return &slab_global.order_map[aligned_4mb / (PAGE_2MB * 2)];
}

uint8_t slab_order_map_get(vaddr_t vaddr) {
    bool get_high;
    uint8_t byte = atomic_load_rlx(order_map_entry_for(vaddr, &get_high));
    if (get_high) {
        return byte >> 4;
    } else {
        return byte & 0xF;
    }
}

void slab_order_map_set(vaddr_t vaddr, uint8_t order) {
    order &= 0xF;
    bool set_high;
    atomic_uint8_t *bptr = order_map_entry_for(vaddr, &set_high);

    uint8_t first_mask = set_high ? 0xF : 0xF0;
    order = set_high ? order << 4 : order;

    atomic_update_explicit(bptr, old_val, (old_val & first_mask) | order,
                           mo_release, mo_rlx);
}

void slab_order_map_init(void) {
    size_t range = SLAB_HEAP_END - SLAB_HEAP_START;
    size_t n4mb_blocks = range / (PAGE_2MB * 2);
    size_t bytes_needed = n4mb_blocks;
    slab_global.order_map = must(simple_alloc(slab_global.vas, bytes_needed));

    uint8_t set_to = SLAB_POW2_ORDER_EMPTY << 4 | SLAB_POW2_ORDER_EMPTY;
    memset(slab_global.order_map, set_to, bytes_needed);
}
