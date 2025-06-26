#include <console/printf.h>
#include <fs/generic.h>
#include <mem/alloc.h>
#include <stdatomic.h>

/* sleeping locks aren't necessary here because there isn't
 * going to be a long wait for cache accesses - hopefullly
 */

void fs_cache_init(struct fs_cache *cache, uint64_t capacity) {
    cache->capacity = capacity;
    cache->count = 0;
    cache->entries = kzalloc(sizeof(struct fs_cache_wrapper) * capacity);
}

/* eviction must be explicitly and separately called */
bool fs_cache_insert(struct fs_cache *cache, uint64_t key,
                     struct fs_cache_entry *value) {

    bool ints = spin_lock(&cache->lock);

    uint64_t index = fs_cache_hash(key, cache->capacity);
    fs_cache_increment_ticks(cache);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct fs_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied || entry->key == key) {
            entry->key = key;
            entry->value = value;
            entry->value->access_time = fs_cache_get_ticks(cache);
            entry->occupied = true;
            cache->count++;
            spin_unlock(&cache->lock, ints);
            return true;
        }
    }

    spin_unlock(&cache->lock, ints);
    return false; // full
}

struct fs_cache_entry *fs_cache_get(struct fs_cache *cache, uint64_t key) {
    bool ints = spin_lock(&cache->lock);

    uint64_t index = fs_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct fs_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied) {
            spin_unlock(&cache->lock, ints);
            return NULL; // not found
        }

        if (entry->key == key) {
            entry->value->access_time = fs_cache_get_ticks(cache);
            spin_unlock(&cache->lock, ints);
            return entry->value;
        }
    }

    spin_unlock(&cache->lock, ints);
    return NULL;
}

bool fs_cache_remove(struct fs_cache *cache, uint64_t key) {
    bool ints = spin_lock(&cache->lock);
    uint64_t index = fs_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct fs_cache_wrapper *entry = &cache->entries[try];

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

bool fs_cache_evict(struct fs_cache *cache) {
    bool ints = spin_lock(&cache->lock);

    /* find oldest accessed cache entry */
    uint64_t oldest = UINT64_MAX;
    uint64_t target = 0;
    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct fs_cache_wrapper *entry = &cache->entries[i];

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
        return fs_cache_remove(cache, target);
    }

    spin_unlock(&cache->lock, ints);
    return false;
}

/* TODO: free all entries */
void fs_cache_destroy(struct fs_cache *cache) {
    kfree(cache->entries);
    cache->entries = NULL;
    cache->capacity = 0;
    cache->count = 0;
}

void fs_cache_ent_lock(struct fs_cache_entry *ent) {
    return mutex_lock(&ent->lock);
}

void fs_cache_ent_unlock(struct fs_cache_entry *ent) {
    return mutex_unlock(&ent->lock);
}
