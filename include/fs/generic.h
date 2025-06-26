#include <mutex.h>
#include <stdbool.h>
#include <stdint.h>
#pragma once
#define MAX_CACHE_ENTRIES 1024

struct fs_cache_entry {
    /* allocated upon new reads */
    uint8_t *buffer;
    uint64_t size;

    /* number of this entry */
    uint64_t number;

    /* used with a counter - not a real 'timestamp' */
    uint64_t access_time;
    struct mutex lock;
    bool dirty;
    bool no_evict;
};

struct fs_cache_wrapper {
    uint64_t key;                 // block number
    struct fs_cache_entry *value; // pointer to cache entry
    bool occupied;
};

struct fs_cache {
    struct fs_cache_wrapper *entries;
    uint64_t capacity;
    uint64_t count;
};

#define FS_CACHE_INIT {0}

static inline uint64_t fs_cache_hash(uint64_t x, uint64_t capacity) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x % capacity;
}

void fs_cache_init(struct fs_cache *cache, uint64_t capacity);
bool fs_cache_insert(struct fs_cache *cache, uint64_t key,
                     struct fs_cache_entry *value);
struct fs_cache_entry *fs_cache_get(struct fs_cache *cache, uint64_t key);
bool fs_cache_remove(struct fs_cache *cache, uint64_t key);
void fs_cache_destroy(struct fs_cache *cache);

