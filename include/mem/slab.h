#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>

#define SLAB_MIN_SHIFT 4
#define SLAB_MAX_SHIFT 18
#define SLAB_CLASS_COUNT (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)

enum slab_state { SLAB_FREE, SLAB_PARTIAL, SLAB_FULL };

struct slab {
    struct slab *next;
    struct slab *prev;

    atomic_uint_fast8_t *bitmap;
    void *mem;
    atomic_uint_fast64_t used;
    enum slab_state state;
    struct slab_cache *parent_cache;
};

struct slab_cache_percore {
    struct slab *current_partial;
    struct slab *current_free;
};

struct slab_cache {
    uint64_t obj_size;
    uint64_t objs_per_slab;
    struct slab *slabs_free;
    struct slab *slabs_partial;
    struct slab *slabs_full;
    struct slab_cache_percore **percore_caches;
};

struct slab_phdr {
    uint32_t magic;
    uint64_t pages;
};

void slab_init();
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
extern uintptr_t slab_heap_top;

#pragma once
