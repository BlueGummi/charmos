#include <mem/elcm.h>

#include "internal.h"

struct slab_elcm_candidate slab_elcm(size_t object_size,
                                     struct slab_elcm_params sep) {
    struct elcm_params params = {
        .metadata_size_bytes = sizeof(struct slab),
        .metadata_bits_per_obj = 1,
        .obj_size = object_size,
        .bias_towards_pow2 = true, 
        .max_wastage_pct = sep.max_wastage_pct,
        .max_pages = sep.max_pages,
    };

    if (elcm(&params) != ERR_OK)
        return (struct slab_elcm_candidate){.pages = 0, .bitmap_size_bytes = 0};

    return (struct slab_elcm_candidate){.pages = params.out.pages,
                                        .bitmap_size_bytes =
                                            params.out.bitmap_bytes};
}
