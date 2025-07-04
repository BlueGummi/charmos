#include <block/bcache.h>
#include <block/generic.h>
#include <block/sched.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spin_lock.h"

#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

static uint8_t *get_lba_offset_buffer(struct bcache_entry *ent, uint64_t lba,
                                      uint64_t spb, uint64_t block_size) {
    uint64_t offset_lba = lba - ent->lba;
    uint64_t offset_bytes = (block_size / spb) * offset_lba;
    return ent->buffer + offset_bytes;
}

static bool remove(struct bcache *cache, uint64_t key, uint64_t spb);

static bool insert(struct bcache *cache, uint64_t key,
                   struct bcache_entry *value);

static struct bcache_entry *get(struct bcache *cache, uint64_t key);
static bool write(struct generic_disk *d, struct bcache *cache,
                  struct bcache_entry *ent, uint64_t spb);

/* prefetch is asynchronous */
static void prefetch(struct generic_disk *disk, struct bcache *cache,
                     uint64_t lba, uint64_t block_size, uint64_t spb);

/* sleeping locks aren't necessary here because there isn't
 * going to be a long wait for cache accesses - hopefullly
 */

void bcache_init(struct bcache *cache, uint64_t capacity) {
    cache->capacity = capacity;
    cache->count = 0;
    cache->entries = kzalloc(sizeof(struct bcache_wrapper) * capacity);
}

/* eviction must be explicitly and separately called */
static bool insert(struct bcache *cache, uint64_t key,
                   struct bcache_entry *value) {

    bool ints = spin_lock(&cache->lock);

    uint64_t index = bcache_hash(key, cache->capacity);
    bcache_increment_ticks(cache);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct bcache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied || entry->key == key) {
            entry->key = key;
            entry->value = value;
            entry->value->access_time = bcache_get_ticks(cache);
            entry->occupied = true;
            cache->count++;
            spin_unlock(&cache->lock, ints);
            return true;
        }
    }

    spin_unlock(&cache->lock, ints);
    return false; // full
}

static struct bcache_entry *get(struct bcache *cache, uint64_t key) {
    bool ints = spin_lock(&cache->lock);

    uint64_t index = bcache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct bcache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied) {
            spin_unlock(&cache->lock, ints);
            return NULL; // not found
        }

        if (entry->key == key) {
            entry->value->access_time = bcache_get_ticks(cache);
            spin_unlock(&cache->lock, ints);
            return entry->value;
        }
    }

    spin_unlock(&cache->lock, ints);
    return NULL;
}

static bool can_remove_lba_group(struct bcache *cache, uint64_t base_lba,
                                 uint64_t spb) {
    /* caller must already hold the lock */
    for (uint64_t i = 0; i < spb; i++) {
        uint64_t key = base_lba + i;
        uint64_t index = bcache_hash(key, cache->capacity);
        bool found = false;

        for (uint64_t j = 0; j < cache->capacity; j++) {
            uint64_t try = (index + j) % cache->capacity;
            struct bcache_wrapper *entry = &cache->entries[try];

            if (!entry->occupied)
                break;

            if (entry->key == key) {
                if (i == 0) {
                    found = true; // base_lba should be found
                } else {
                    return false; // another lba in the group is still cached
                }
                break;
            }
        }

        if (i == 0 && !found)
            return false; // base_lba must exist
    }

    return true;
}

static bool remove(struct bcache *cache, uint64_t key, uint64_t spb) {
    bool ints = spin_lock(&cache->lock);
    uint64_t index = bcache_hash(key, cache->capacity);

    for (uint64_t i = 0; i < cache->capacity; i++) {
        uint64_t try = (index + i) % cache->capacity;
        struct bcache_wrapper *entry = &cache->entries[try];

        if (!entry->occupied) {
            spin_unlock(&cache->lock, ints);
            return false;
        }

        if (entry->key == key) {
            struct bcache_entry *val = entry->value;
            entry->occupied = false;
            entry->value = NULL;
            cache->count--;

            bool should_free = false;
            if (val && key == val->lba && !val->no_evict) {
                if (can_remove_lba_group(cache, key, val->size / spb)) {
                    should_free = true;
                }
            }

            spin_unlock(&cache->lock, ints);

            if (should_free) {
                kfree_aligned(val->buffer);
                kfree(val);
            }

            return true;
        }
    }

    spin_unlock(&cache->lock, ints);
    return false;
}

struct bcache_pf_data {
    struct bcache_entry *new_entry;
    struct bcache *cache;
};

static void prefetch_callback(struct bio_request *bio) {
    struct bcache_pf_data *data = bio->user_data;
    struct bcache_entry *ent = data->new_entry;
    struct bcache *cache = data->cache;
    ent->buffer = bio->buffer;
    ent->lba = bio->lba;
    ent->dirty = false;
    ent->no_evict = false;
    ent->size = bio->size;
    insert(cache, bio->lba, ent);
    kfree(data);
    kfree(bio);
}

static void prefetch(struct generic_disk *disk, struct bcache *cache,
                     uint64_t lba, uint64_t block_size, uint64_t spb) {

    uint64_t base_lba = ALIGN_DOWN(lba, spb);

    /* no need to re-fetch existing entry */
    if (get(cache, base_lba))
        return;

    struct bio_request *req = kmalloc(sizeof(struct bio_request));
    struct bcache_pf_data *pf = kmalloc(sizeof(struct bcache_pf_data));
    pf->cache = cache;
    pf->new_entry = kmalloc(sizeof(struct bcache_entry));

    req->buffer = kmalloc_aligned(block_size, PAGE_SIZE);
    req->disk = disk;
    req->lba = base_lba;
    req->done = false;
    req->sector_count = spb;
    req->status = -1;
    req->write = false;
    req->user_data = pf;
    req->on_complete = prefetch_callback;

    disk->submit_bio_async(disk, req);
}

void bcache_prefetch_async(struct generic_disk *disk, uint64_t lba,
                           uint64_t block_size, uint64_t spb) {
    uint64_t aligned = ALIGN_DOWN(lba, spb);

    prefetch(disk, disk->cache, aligned, block_size, spb);
}

static bool evict(struct bcache *cache, uint64_t spb) {
    bool ints = spin_lock(&cache->lock);

    /* find oldest accessed cache entry */
    uint64_t oldest = UINT64_MAX;
    uint64_t target = 0;
    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct bcache_wrapper *entry = &cache->entries[i];

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
        return remove(cache, target, spb);
    }

    spin_unlock(&cache->lock, ints);
    return false;
}

/* TODO: writeback */
static bool write(struct generic_disk *d, struct bcache *cache,
                  struct bcache_entry *ent, uint64_t spb) {
    bool ints = spin_lock(&cache->lock);
    bool ret = d->write_sector(d, ent->lba, ent->buffer, spb);
    spin_unlock(&cache->lock, ints);
    return ret;
}

/* TODO: free all entries */
void bcache_destroy(struct bcache *cache) {
    kfree(cache->entries);
    cache->entries = NULL;
    cache->capacity = 0;
    cache->count = 0;
}

struct bcache_entry *bcache_get(struct generic_disk *disk, uint64_t lba,
                                uint64_t block_size, uint64_t spb,
                                bool no_evict) {
    uint64_t base_lba = ALIGN_DOWN(lba, spb);
    struct bcache_entry *ent = get(disk->cache, base_lba);

    if (ent) {
        struct bcache_entry *shallow = kzalloc(sizeof(struct bcache_entry));
        *shallow = *ent;
        shallow->buffer = get_lba_offset_buffer(ent, lba, spb, block_size);
        shallow->lba = lba;
        return shallow;
    }

    ent = bcache_create_ent(disk, lba, block_size, spb, no_evict);
    return ent;
}

bool bcache_insert(struct generic_disk *disk, uint64_t lba,
                   struct bcache_entry *ent, uint64_t spb) {
    if (insert(disk->cache, lba, ent)) {
        return true;
    } else {
        evict(disk->cache, spb);
        return insert(disk->cache, lba, ent);
    }
}

bool bcache_evict(struct generic_disk *disk, uint64_t spb) {
    return evict(disk->cache, spb);
}

bool bcache_write(struct generic_disk *disk, struct bcache_entry *ent,
                  uint64_t spb) {
    return write(disk, disk->cache, ent, spb);
}

struct bcache_entry *bcache_create_ent(struct generic_disk *disk, uint64_t lba,
                                       uint64_t block_size,
                                       uint64_t sectors_per_block,
                                       bool no_evict) {
    uint64_t base_lba = ALIGN_DOWN(lba, sectors_per_block);

    // check if it already exists
    struct bcache_entry *existing = get(disk->cache, base_lba);
    if (existing) {
        struct bcache_entry *shallow = kzalloc(sizeof(struct bcache_entry));
        *shallow = *existing;
        shallow->buffer =
            get_lba_offset_buffer(existing, lba, sectors_per_block, block_size);
        shallow->lba = lba;
        return shallow;
    }

    uint8_t *buf = kmalloc_aligned(block_size, PAGE_SIZE);
    if (!disk->read_sector(disk, base_lba, buf, sectors_per_block)) {
        kfree_aligned(buf);
        return NULL;
    }

    struct bcache_entry *ent = kzalloc(sizeof(struct bcache_entry));
    ent->buffer = buf;
    ent->lba = base_lba;
    ent->size = block_size;
    ent->no_evict = no_evict;

    bcache_insert(disk, base_lba, ent, sectors_per_block);

    struct bcache_entry *shallow = kzalloc(sizeof(struct bcache_entry));
    *shallow = *ent;
    shallow->buffer =
        get_lba_offset_buffer(ent, lba, sectors_per_block, block_size);
    shallow->lba = lba;
    return shallow;
}

void bcache_ent_lock(struct bcache_entry *ent) {
    return mutex_lock(&ent->lock);
}

void bcache_ent_unlock(struct bcache_entry *ent) {
    return mutex_unlock(&ent->lock);
}
