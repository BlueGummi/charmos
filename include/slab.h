#include <stddef.h>
#include <stdint.h>

#define SLAB_MIN_SHIFT 4
#define SLAB_MAX_SHIFT 12
#define SLAB_CLASS_COUNT (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)
#define UACPI_MAP_BASE 0xFFFFA00000000000
#define UACPI_MAP_LIMIT 0xFFFFA00000100000
#define KMALLOC_PAGE_MAGIC 0xC0FFEE42

enum slab_state { SLAB_FREE, SLAB_PARTIAL, SLAB_FULL };

struct slab {
    struct slab *next;
    uint8_t *bitmap;
    void *mem;
    size_t used;
    enum slab_state state;
    struct slab_cache *parent_cache;
};

struct slab_cache {
    size_t obj_size;
    size_t objs_per_slab;
    struct slab *slabs_free;
    struct slab *slabs_partial;
    struct slab *slabs_full;
};

struct kmalloc_page_header {
    uint32_t magic;
    size_t pages;
};
void slab_init();
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
extern uintptr_t slab_heap_top;

#pragma once
