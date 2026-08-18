#pragma once
#include <test_export.h>

TEST_IMPORT(void *, slab_map_new, struct slab_cache *cache,
            paddr_t phys_out[SLAB_MAX_PAGES], struct slab_chunk **out);
