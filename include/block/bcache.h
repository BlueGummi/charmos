#include <block/bio.h>
#include <mutex.h>
#include <refcount.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

#pragma once
#define DEFAULT_BLOCK_CACHE_SIZE 2048
#define DEFAULT_MAX_DIRTY_ENTS 64

struct generic_disk;

/* must be allocated with malloc */
struct bcache_entry {
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

    /* associated outgoing request */
    struct bio_request *request;

    refcount_t refcount;
};

struct bcache_wrapper {
    uint64_t key;               // block number
    struct bcache_entry *value; // pointer to cache entry
    bool occupied;
};

/* TODO: bitmap to mark present entries */
struct bcache {
    struct bcache_wrapper *entries;
    atomic_uint_fast64_t ticks;
    uint64_t capacity;
    uint64_t count;
    uint64_t spb;
    struct spinlock lock;
};

static inline uint64_t bcache_hash(uint64_t x, uint64_t capacity) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x % capacity;
}

void bcache_init(struct bcache *cache, uint64_t capacity);

void *bcache_get(struct generic_disk *disk, uint64_t lba, uint64_t block_size,
                 uint64_t spb, bool no_evict, struct bcache_entry **out_entry);

bool bcache_writethrough(struct generic_disk *disk, struct bcache_entry *ent,
                         uint64_t spb);

void bcache_write_queue(struct generic_disk *disk, struct bcache_entry *ent,
                        uint64_t spb, enum bio_request_priority prio);

bool bcache_insert(struct generic_disk *disk, uint64_t lba,
                   struct bcache_entry *ent, uint64_t spb);

bool bcache_evict(struct generic_disk *disk, uint64_t spb);

void bcache_prefetch_async(struct generic_disk *disk, uint64_t lba,
                           uint64_t block_size, uint64_t spb);

void *bcache_create_ent(struct generic_disk *disk, uint64_t lba,
                        uint64_t block_size, uint64_t sectors_per_block,
                        bool no_evict, struct bcache_entry **out_entry);

static inline void bcache_increment_ticks(struct bcache *cache) {
    atomic_fetch_add(&cache->ticks, 1);
}

static inline uint64_t bcache_get_ticks(struct bcache *cache) {
    return atomic_load(&cache->ticks);
}

static inline void bcache_ent_lock(struct bcache_entry *ent) {
    mutex_lock(&ent->lock);
}

static inline void bcache_ent_unlock(struct bcache_entry *ent) {
    mutex_unlock(&ent->lock);
}

static inline void bcache_ent_pin(struct bcache_entry *ent) {
    refcount_inc(&ent->refcount);
}

static inline void bcache_ent_unpin(struct bcache_entry *ent) {
    refcount_dec_and_test(&ent->refcount);
}

static inline void bcache_ent_acquire(struct bcache_entry *ent) {
    bcache_ent_pin(ent);
    bcache_ent_lock(ent);
}

static inline void bcache_ent_release(struct bcache_entry *ent) {
    bcache_ent_unpin(ent);
    bcache_ent_unlock(ent);
}
