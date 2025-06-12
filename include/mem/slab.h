#include <stddef.h>
#include <stdint.h>

#define SLAB_MIN_SHIFT 4
#define SLAB_MAX_SHIFT 12
#define SLAB_CLASS_COUNT (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)

enum slab_state { SLAB_FREE, SLAB_PARTIAL, SLAB_FULL };

struct slab {
    struct slab *next;
    uint8_t *bitmap;
    void *mem;
    uint64_t used;
    enum slab_state state;
    struct slab_cache *parent_cache;
};

struct slab_cache {
    uint64_t obj_size;
    uint64_t objs_per_slab;
    struct slab *slabs_free;
    struct slab *slabs_partial;
    struct slab *slabs_full;
};

struct slab_phdr {
    uint32_t magic;
    uint64_t pages;
};

void slab_init();
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
extern uintptr_t slab_heap_top;

#pragma once
