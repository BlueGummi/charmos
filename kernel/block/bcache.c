#include <block/bcache.h>
#include <block/generic.h>
#include <block/sched.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TODO: File is getting big, split into multiple */

/* TODO: use linked list in hashmap collisions */

#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

static bool remove(struct bcache *cache, uint64_t key, uint64_t spb);

static bool insert(struct bcache *cache, uint64_t key,
                   struct bcache_entry *value, bool already_locked);

static struct bcache_entry *get(struct bcache *cache, uint64_t key);
static bool write(struct generic_disk *d, struct bcache *cache,
                  struct bcache_entry *ent, uint64_t spb);

/* prefetch is asynchronous */
static enum errno prefetch(struct generic_disk *disk, struct bcache *cache,
                           uint64_t lba, uint64_t block_size, uint64_t spb);

/* eviction must be explicitly and separately called */
static bool insert(struct bcache *cache, uint64_t key,
                   struct bcache_entry *value, bool already_locked) {

    bool ints = false;

    if (!already_locked)
        ints = spin_lock(&cache->lock);

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

            if (!already_locked)
                spin_unlock(&cache->lock, ints);
            return true;
        }
    }

    if (!already_locked)
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

            if (should_free) {
                kfree_aligned(val->buffer);
                kfree(val);
            }

            spin_unlock(&cache->lock, ints);
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
    insert(data->cache, bio->lba, data->new_entry, false);

    defer_free(data);
    defer_free(bio);
}

static enum errno prefetch(struct generic_disk *disk, struct bcache *cache,
                           uint64_t lba, uint64_t block_size, uint64_t spb) {
    uint64_t base_lba = ALIGN_DOWN(lba, spb);

    /* no need to re-fetch existing entry */
    if (get(cache, base_lba))
        return ERR_EXIST;

    struct bcache_pf_data *pf = kmalloc(sizeof(struct bcache_pf_data));
    if (!pf)
        return ERR_NO_MEM;

    struct bio_request *req = bio_create_read(disk, base_lba, spb, block_size,
                                              prefetch_callback, pf, NULL);

    pf->cache = cache;
    pf->new_entry = kzalloc(sizeof(struct bcache_entry));
    if (!pf->new_entry)
        return ERR_NO_MEM;

    struct bcache_entry *ent = pf->new_entry;

    ent->lba = lba;
    ent->size = block_size;
    ent->dirty = false;
    ent->no_evict = false;
    ent->buffer = req->buffer;

    bio_sched_enqueue(disk, req);
    return ERR_OK;
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

        if (entry->value->no_evict || atomic_load(&entry->value->refcount) > 0)
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

static void stat(struct bcache *cache, uint64_t *total_dirty_out,
                 uint64_t *total_present_out) {
    bool ints = spin_lock(&cache->lock);

    uint64_t total_dirty = 0;
    uint64_t total_present = 0;
    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct bcache_wrapper *entry = &cache->entries[i];

        if (!entry->occupied || !entry->value)
            continue;

        total_present++;

        if (entry->value->dirty) {
            total_dirty++;
        }
    }

    if (total_dirty_out)
        *total_dirty_out = total_dirty;

    if (total_present_out)
        *total_present_out = total_present;

    spin_unlock(&cache->lock, ints);
}

/* TODO: writeback */
static bool write(struct generic_disk *d, struct bcache *cache,
                  struct bcache_entry *ent, uint64_t spb) {
    bool ints = spin_lock(&cache->lock);

    bool ret = d->write_sector(d, ent->lba, ent->buffer, spb);
    uint64_t aligned = ALIGN_DOWN(ent->lba, spb);
    if (aligned != ent->lba)
        kfree(ent);

    spin_unlock(&cache->lock, ints);
    return ret;
}

static void write_enqueue_cb(struct bio_request *req) {
    struct bcache_entry *ent = req->user_data;

    ent->dirty = false;
    ent->request = NULL;
    bcache_ent_unpin(ent);

    defer_free(req);
}

static void write_queue(struct generic_disk *d, struct bcache_entry *ent,
                        uint64_t spb, enum bio_request_priority prio) {

    ent->dirty = true;

    struct bio_request *req = bio_create_write(
        d, ent->lba, spb, ent->size, write_enqueue_cb, ent, ent->buffer);

    req->priority = prio;
    ent->request = req;

    bcache_ent_pin(ent);
    bio_sched_enqueue(d, req);
}

/* TODO: free all entries */
void bcache_destroy(struct bcache *cache) {
    kfree(cache->entries);
    cache->entries = NULL;
    cache->capacity = 0;
    cache->count = 0;
}

void *bcache_get(struct generic_disk *disk, uint64_t lba, uint64_t block_size,
                 uint64_t spb, bool no_evict, struct bcache_entry **out_entry) {
    uint64_t base_lba = ALIGN_DOWN(lba, spb);
    struct bcache_entry *ent = get(disk->cache, base_lba);

    if (!ent)
        return bcache_create_ent(disk, lba, block_size, spb, no_evict,
                                 out_entry);

    *out_entry = ent;

    uint64_t offset = (lba - base_lba) * disk->sector_size;
    return ent->buffer + offset;
}

bool bcache_insert(struct generic_disk *disk, uint64_t lba,
                   struct bcache_entry *ent, uint64_t spb) {
    if (insert(disk->cache, lba, ent, false)) {
        return true;
    } else {
        evict(disk->cache, spb);
        return insert(disk->cache, lba, ent, false);
    }
}

bool bcache_evict(struct generic_disk *disk, uint64_t spb) {
    return evict(disk->cache, spb);
}

bool bcache_writethrough(struct generic_disk *disk, struct bcache_entry *ent,
                         uint64_t spb) {
    return write(disk, disk->cache, ent, spb);
}

void bcache_write_queue(struct generic_disk *disk, struct bcache_entry *ent,
                        uint64_t spb, enum bio_request_priority prio) {
    write_queue(disk, ent, spb, prio);
}

void bcache_stat(struct generic_disk *disk, uint64_t *total_dirty_out,
                 uint64_t *total_present_out) {
    stat(disk->cache, total_dirty_out, total_present_out);
}

/* TODO: error code here */
void *bcache_create_ent(struct generic_disk *disk, uint64_t lba,
                        uint64_t block_size, uint64_t sectors_per_block,
                        bool no_evict, struct bcache_entry **out_entry) {
    uint64_t base_lba = ALIGN_DOWN(lba, sectors_per_block);

    struct bcache_entry *ent = get(disk->cache, base_lba);
    bool i = spin_lock(&disk->cache->lock);

    if (!ent) {
        uint8_t *buf = kmalloc_aligned(block_size, PAGE_SIZE);
        if (!buf)
            return NULL;

        if (!disk->read_sector(disk, base_lba, buf, sectors_per_block)) {
            kfree_aligned(buf);
            spin_unlock(&disk->cache->lock, i);
            *out_entry = NULL;
            return NULL;
        }

        ent = kzalloc(sizeof(struct bcache_entry));
        if (!ent)
            return NULL;

        ent->buffer = buf;
        ent->lba = base_lba;
        ent->size = block_size;
        ent->no_evict = no_evict;

        if (!insert(disk->cache, base_lba, ent, true)) {
            evict(disk->cache, sectors_per_block);
            insert(disk->cache, base_lba, ent, true);
        }
    }

    spin_unlock(&disk->cache->lock, i);

    *out_entry = ent;

    uint64_t offset = (lba - base_lba) * disk->sector_size;
    return ent->buffer + offset;
}

enum errno bcache_prefetch_async(struct generic_disk *disk, uint64_t lba,
                                 uint64_t block_size, uint64_t spb) {
    return prefetch(disk, disk->cache, ALIGN_DOWN(lba, spb), block_size, spb);
}

void bcache_init(struct bcache *cache, uint64_t capacity) {
    spinlock_init(&cache->lock);
    cache->capacity = capacity;
    cache->count = 0;
    cache->entries = kzalloc(sizeof(struct bcache_wrapper) * capacity);
    if (!cache->entries)
        k_panic("Block cache initialization allocation failed\n");
}
