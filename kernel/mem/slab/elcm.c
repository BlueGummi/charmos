#include <mem/elcm.h>
#include <mem/slab.h>

#include "internal.h"

static bool initialized = false;

static void *elcm_simple_alloc(size_t size) {
    return simple_alloc(slab_global.vas, size);
}

static void elcm_simple_free(void *ptr, size_t size) {
    simple_free(slab_global.vas, ptr, size);
}

struct slab_elcm_candidate slab_elcm(size_t obj_size, size_t obj_alignment) {
    kassert(obj_size && obj_alignment);
    struct elcm_params params = {
        .metadata_size_bytes = sizeof(struct slab),
#ifdef DEBUG_SLAB_DEEP
        .metadata_bits_per_obj = 1 + sizeof(stack_handle_t) * 8,
#else
        .metadata_bits_per_obj = 1,
#endif
        .obj_size = obj_size,
        .obj_alignment = obj_alignment,
        .bias_towards_pow2 = true,
        .max_wastage_pct = SLAB_ELCM_DEFAULT_MAX_WASTAGE_PCT,
        .max_pages = SLAB_MAX_PAGES,
        .metadata_bytes_per_page = sizeof(struct page *),
        .alloc_fn = initialized ? NULL : elcm_simple_alloc,
        .free_fn = initialized ? NULL : elcm_simple_free,
    };

    if (elcm(&params) != ERR_OK)
        return (struct slab_elcm_candidate){.pages = 0, .bitmap_size_bytes = 0};

    struct elcm_candidate elc = params.out;

    return (struct slab_elcm_candidate){
        .pages = elc.pages,
        .bitmap_size_bytes = SLAB_BITMAP_BYTES_FOR(elc.obj_count),
        .obj_count = elc.obj_count};
}

void slab_elcm_initialize() {
    initialized = true;
}
