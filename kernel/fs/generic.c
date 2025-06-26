#include <console/printf.h>
#include <fs/generic.h>
#include <mem/alloc.h>

void fs_cache_init(struct fs_cache *cache, uint64_t capacity) {
    cache->capacity = capacity;
    cache->count = 0;
    cache->entries = kzalloc(sizeof(struct fs_cache_wrapper) * capacity);
}

bool fs_cache_insert(struct fs_cache *cache, uint64_t key,
                     struct fs_cache_entry *value) {
    uint64_t index = fs_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct fs_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied || entry->key == key) {
            entry->key = key;
            entry->value = value;
            entry->occupied = true;
            cache->count++;
            return true;
        }
    }

    return false; // full
}

struct fs_cache_entry *fs_cache_get(struct fs_cache *cache, uint64_t key) {
    uint64_t index = fs_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct fs_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied)
            return NULL; // not found
        if (entry->key == key)
            return entry->value;
    }

    return NULL;
}

bool fs_cache_remove(struct fs_cache *cache, uint64_t key) {
    uint64_t index = fs_cache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct fs_cache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied)
            return false;
        if (entry->key == key) {
            entry->occupied = false;
            entry->value = NULL;
            cache->count--;
            return true;
        }
    }

    return false;
}

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
