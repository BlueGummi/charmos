#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/bcache.h>
#include <mem/alloc.h>
#include <stdatomic.h>

/* sleeping locks aren't necessary here because there isn't
 * going to be a long wait for cache accesses - hopefullly
 */

void block_cache_init(struct block_cache *cache, uint64_t capacity) {
    cache->capacity = capacity;
    cache->count = 0;
    cache->entries = kzalloc(sizeof(struct block_cache_wrapper) * capacity);
}

/* eviction must be explicitly and separately called */
bool block_cache_insert(struct block_cache *cache, uint64_t key,
                        struct block_cache_entry *value) {

    bool ints = spin_lock(&cache->lock);

    uint64_t index = block_cache_hash(key, cache->capacity);
    block_cache_increment_ticks(cache);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct block_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied || entry->key == key) {
            entry->key = key;
            entry->value = value;
            entry->value->access_time = block_cache_get_ticks(cache);
            entry->occupied = true;
            cache->count++;
            spin_unlock(&cache->lock, ints);
            return true;
        }
    }

    spin_unlock(&cache->lock, ints);
    return false; // full
}

struct block_cache_entry *block_cache_get(struct block_cache *cache,
                                          uint64_t key) {
    bool ints = spin_lock(&cache->lock);

    uint64_t index = block_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct block_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied) {
            spin_unlock(&cache->lock, ints);
            return NULL; // not found
        }

        if (entry->key == key) {
            entry->value->access_time = block_cache_get_ticks(cache);
            spin_unlock(&cache->lock, ints);
            return entry->value;
        }
    }

    spin_unlock(&cache->lock, ints);
    return NULL;
}

bool block_cache_remove(struct block_cache *cache, uint64_t key) {
    bool ints = spin_lock(&cache->lock);
    uint64_t index = block_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct block_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied) {
            spin_unlock(&cache->lock, ints);
            return false;
        }

        if (entry->key == key) {
            entry->occupied = false;
            entry->value = NULL;
            cache->count--;

            kfree(entry->value->buffer);
            kfree(entry->value);
            spin_unlock(&cache->lock, ints);
            return true;
        }
    }

    spin_unlock(&cache->lock, ints);
    return false;
}

bool block_cache_evict(struct block_cache *cache) {
    bool ints = spin_lock(&cache->lock);

    /* find oldest accessed cache entry */
    uint64_t oldest = UINT64_MAX;
    uint64_t target = 0;
    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct block_cache_wrapper *entry = &cache->entries[i];

        if (!entry->occupied || !entry->value)
            continue;

        if (entry->value->no_evict)
            continue;

        if (entry->value->access_time < oldest) {
            oldest = entry->value->access_time;
            target = entry->key;
        }
    }

    if (target) {
        spin_unlock(&cache->lock, ints);
        return block_cache_remove(cache, target);
    }

    spin_unlock(&cache->lock, ints);
    return false;
}

/* TODO: free all entries */
void block_cache_destroy(struct block_cache *cache) {
    kfree(cache->entries);
    cache->entries = NULL;
    cache->capacity = 0;
    cache->count = 0;
}

void block_cache_ent_lock(struct block_cache_entry *ent) {
    return mutex_lock(&ent->lock);
}

void block_cache_ent_unlock(struct block_cache_entry *ent) {
    return mutex_unlock(&ent->lock);
}

struct block_cache_entry *bcache_get(struct generic_disk *disk, uint64_t lba,
                                     uint64_t block_size, uint64_t spb,
                                     bool no_evict) {
    struct block_cache_entry *ret = block_cache_get(disk->cache, lba);

    if (ret) {
        return ret;
    }

    ret = bcache_create_ent(disk, lba, block_size, spb, no_evict);

    bool status = bcache_insert(disk, lba, ret);

    /* insertion does not call eviction */
    if (!status) {
        bcache_evict(disk);
        bcache_insert(disk, lba, ret);
    }
    return ret;
}

bool bcache_insert(struct generic_disk *disk, uint64_t lba,
                   struct block_cache_entry *ent) {
    return block_cache_insert(disk->cache, lba, ent);
}

bool bcache_evict(struct generic_disk *disk) {
    return block_cache_evict(disk->cache);
}

struct block_cache_entry *bcache_create_ent(struct generic_disk *disk,
                                            uint64_t lba, uint64_t block_size,
                                            uint64_t sectors_per_block,
                                            bool no_evict) {
    uint8_t *buf = kmalloc(block_size);

    if (!disk->read_sector(disk, lba, buf, sectors_per_block))
        return NULL;

    struct block_cache_entry *ent = kzalloc(sizeof(struct block_cache_entry));
    ent->buffer = buf;
    ent->lba = lba;
    ent->size = block_size;
    ent->no_evict = no_evict;
    bcache_insert(disk, lba, ent);
    return ent;
}
