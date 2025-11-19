#include <block/bcache.h>
#include <block/generic.h>
#include <block/sched.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    enum irql irql;
    if (!already_locked)
        irql = spin_lock(&cache->lock);

    uint64_t index = bcache_hash(key, cache->capacity);
    struct bcache_wrapper *head = cache->entries[index];

    /* Key already exists */
    for (struct bcache_wrapper *node = head; node; node = node->next) {
        if (node->key == key) {
            node->value = value;
            node->value->access_time = bcache_get_ticks(cache);
            if (!already_locked)
                spin_unlock(&cache->lock, irql);
            return true;
        }
    }

    /* New head */
    struct bcache_wrapper *new_node = kmalloc(sizeof(struct bcache_wrapper));
    if (!new_node) {
        if (!already_locked)
            spin_unlock(&cache->lock, irql);
        return false;
    }

    new_node->key = key;
    new_node->value = value;
    new_node->value->access_time = bcache_get_ticks(cache);
    new_node->next = head;

    cache->entries[index] = new_node;
    cache->count++;

    if (!already_locked)
        spin_unlock(&cache->lock, irql);
    return true;
}

static struct bcache_entry *get(struct bcache *cache, uint64_t key) {
    enum irql irql = spin_lock(&cache->lock);

    uint64_t index = bcache_hash(key, cache->capacity);
    struct bcache_wrapper *node = cache->entries[index];

    while (node) {
        if (node->key == key) {
            node->value->access_time = bcache_get_ticks(cache);
            spin_unlock(&cache->lock, irql);
            return node->value;
        }
        node = node->next;
    }

    spin_unlock(&cache->lock, irql);
    return NULL;
}

static bool can_remove_lba_group(struct bcache *cache, uint64_t base_lba,
                                 uint64_t spb) {
    /* caller must already hold the lock */

    for (uint64_t i = 0; i < spb; i++) {
        uint64_t key = base_lba + i;
        uint64_t index = bcache_hash(key, cache->capacity);
        struct bcache_wrapper *node = cache->entries[index];
        bool found = false;

        while (node) {
            if (node->key == key) {
                found = true;
                break;
            }
            node = node->next;
        }

        if (i == 0 && !found)
            return false; /* base_lba must exist */
        else if (i > 0 && found)
            return false; /* another lba in the group is still cached */
    }

    return true;
}

static bool remove(struct bcache *cache, uint64_t key, uint64_t spb) {
    enum irql irql = spin_lock(&cache->lock);
    uint64_t index = bcache_hash(key, cache->capacity);

    struct bcache_wrapper *prev = NULL;
    struct bcache_wrapper *node = cache->entries[index];

    while (node) {
        if (node->key == key) {
            if (prev)
                prev->next = node->next;
            else
                cache->entries[index] = node->next;

            struct bcache_entry *val = node->value;
            cache->count--;
            kfree(node);

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

            spin_unlock(&cache->lock, irql);
            return true;
        }

        prev = node;
        node = node->next;
    }

    spin_unlock(&cache->lock, irql);
    return false;
}

struct bcache_pf_data {
    struct bcache_entry *new_entry;
    struct bcache *cache;
};

static void prefetch_callback(struct bio_request *bio) {
    struct bcache_pf_data *data = bio->user_data;
    insert(data->cache, bio->lba, data->new_entry, false);

    kfree(data);
    kfree(bio);
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
    if (!req)
        return ERR_NO_MEM;

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
    enum irql irql = spin_lock(&cache->lock);

    uint64_t oldest = UINT64_MAX;
    uint64_t target_key = 0;
    bool found = false;

    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct bcache_wrapper *node = cache->entries[i];
        while (node) {
            struct bcache_entry *entry = node->value;

            if (!entry || entry->no_evict ||
                atomic_load(&entry->refcount) > 0) {
                node = node->next;
                continue;
            }

            if (entry->access_time < oldest) {
                oldest = entry->access_time;
                target_key = node->key;
                found = true;
            }

            node = node->next;
        }
    }

    spin_unlock(&cache->lock, irql);

    if (found)
        return remove(cache, target_key, spb);

    return false;
}

static void stat(struct bcache *cache, uint64_t *total_dirty_out,
                 uint64_t *total_present_out) {
    enum irql irql = spin_lock(&cache->lock);

    uint64_t total_dirty = 0;
    uint64_t total_present = 0;

    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct bcache_wrapper *node = cache->entries[i];
        while (node) {
            if (node->value) {
                total_present++;
                if (node->value->dirty)
                    total_dirty++;
            }
            node = node->next;
        }
    }

    if (total_dirty_out)
        *total_dirty_out = total_dirty;

    if (total_present_out)
        *total_present_out = total_present;

    spin_unlock(&cache->lock, irql);
}

/* TODO: writeback */
static bool write(struct generic_disk *d, struct bcache *cache,
                  struct bcache_entry *ent, uint64_t spb) {
    enum irql irql = spin_lock(&cache->lock);

    bool ret = d->write_sector(d, ent->lba, ent->buffer, spb);
    uint64_t aligned = ALIGN_DOWN(ent->lba, spb);
    if (aligned != ent->lba)
        kfree(ent);

    spin_unlock(&cache->lock, irql);
    return ret;
}

static void write_enqueue_cb(struct bio_request *req) {
    struct bcache_entry *ent = req->user_data;

    ent->dirty = false;
    ent->request = NULL;
    bcache_ent_unpin(ent);

    kfree(req);
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

void bcache_destroy(struct bcache *cache) {
    for (uint64_t i = 0; i < cache->capacity; i++) {
        struct bcache_wrapper *node = cache->entries[i];
        while (node) {
            struct bcache_wrapper *next = node->next;
            if (node->value) {
                kfree_aligned(node->value->buffer);
                kfree(node->value);
            }
            kfree(node);
            node = next;
        }
    }

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
    if (!ent) {
        uint8_t *buf = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!buf)
            return NULL;

        if (!disk->read_sector(disk, base_lba, buf, sectors_per_block)) {
            kfree_aligned(buf);
            *out_entry = NULL;
            return NULL;
        }

        ent = kzalloc(sizeof(struct bcache_entry));
        if (!ent)
            return NULL;

        mutex_old_init(&ent->lock);

        ent->buffer = buf;
        ent->lba = base_lba;
        ent->size = block_size;
        ent->no_evict = no_evict;

        bcache_insert(disk, base_lba, ent, sectors_per_block);
    }

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
    cache->entries = kzalloc(sizeof(struct bcache_wrapper *) * capacity);
    if (!cache->entries)
        k_panic("Block cache initialization allocation failed\n");
}
