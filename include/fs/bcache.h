#include <mutex.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#pragma once
#define DEFAULT_BLOCK_CACHE_SIZE 2048

struct generic_disk;

/* must be allocated with malloc */
struct block_cache_entry {
    /* allocated upon new reads */
    uint8_t *buffer;
    uint64_t size;

    /* logical block address */
    uint64_t lba;

    /* used with a counter - not a real 'timestamp' */
    uint64_t access_time;
    struct mutex lock;
    bool dirty;
    bool no_evict;
};

struct block_cache_wrapper {
    uint64_t key;                    // block number
    struct block_cache_entry *value; // pointer to cache entry
    bool occupied;
};

struct block_cache {
    struct block_cache_wrapper *entries;
    atomic_uint_fast64_t ticks;
    uint64_t capacity;
    uint64_t count;
    uint64_t spb;
    struct spinlock lock;
};

static inline uint64_t block_cache_hash(uint64_t x, uint64_t capacity) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x % capacity;
}

void bcache_init(struct block_cache *cache, uint64_t capacity);
void bcache_ent_unlock(struct block_cache_entry *ent);
void bcache_ent_lock(struct block_cache_entry *ent);
struct block_cache_entry *bcache_get(struct generic_disk *disk, uint64_t lba,
                                     uint64_t block_size, uint64_t spb, bool);

bool bcache_insert(struct generic_disk *disk, uint64_t lba,
                   struct block_cache_entry *ent);
bool bcache_evict(struct generic_disk *disk, uint64_t spb);

struct block_cache_entry *bcache_create_ent(struct generic_disk *disk,
                                            uint64_t lba, uint64_t block_size,
                                            uint64_t sectors_per_block,
                                            bool no_evict);

static inline void block_cache_increment_ticks(struct block_cache *cache) {
    atomic_fetch_add(&cache->ticks, 1);
}

static inline uint64_t block_cache_get_ticks(struct block_cache *cache) {
    return atomic_load(&cache->ticks);
}
