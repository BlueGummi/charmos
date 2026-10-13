#include <math/fixed.h>
#include <math/hash.h>
#include <math/sort.h>
#include <mem/alloc.h>
#include <mem/arena_provider.h>
#include <structures/list_rcu.h>
#include <sync/rcu.h>

#include "internal.h"

struct arena_globals arena_global = {0};
enum err arena_budget_prio_scale(sz_b_t used, int scale,
                                 ALLOC_PRIORITY_BITMAP_DECLARE(prio_map_out)) {
    fx32_32_t y = fx_pow_i32(FX_E, (int) used + scale);
    if (y > fx_from_int(ALLOC_PRIORITY_MAX) || y < FX_ZERO)
        return ERR_OVERFLOW;

    bitmap_zero(prio_map_out, ALLOC_PRIORITY_MAX);
    uint16_t base = (uint16_t) fx_to_int(y);
    for (uint16_t i = base; i < ALLOC_PRIORITY_MAX; i++)
        bitmap_set(prio_map_out, i);

    return ERR_OK;
}

static void arena_desc_table_free_cb(struct rcu_cb *cb) {
    struct arena_desc_table *tbl =
        container_of(cb, struct arena_desc_table, rcu_cb);
    vfree(tbl->descs);
    vfree(tbl);
}

static int32_t arena_desc_cmp(const void *a, const void *b) {
    const struct arena_desc *ada = a;
    const struct arena_desc *adb = b;
    return ada->strategy - adb->strategy;
}

enum err arena_desc_register(struct arena_desc *desc) {
    enum err err = ERR_OK;

    enum irql irql = spin_lock(&arena_global.tbl_lock);

    struct arena_desc_table *old_tbl = rcu_dereference(arena_global.desc_table);
    enforce(old_tbl);

    size_t old_desc_count = old_tbl->desc_count;
    size_t desc_count = old_desc_count + 1;
    struct arena_desc_table *new_tbl = vnew(new_tbl, ALLOC_ZERO);
    if (!new_tbl) {
        err = ERR_NO_MEM;
        goto out;
    }

    struct arena_desc **new_descs = vnews(new_descs, desc_count, ALLOC_ZERO);
    if (!new_descs) {
        err = ERR_NO_MEM;
        vfree(new_tbl);
        goto out;
    }

    if (old_desc_count && old_tbl->descs)
        memcpy(new_descs, old_tbl->descs,
               old_desc_count * sizeof(struct arena_desc *));

    new_descs[old_desc_count] = desc;
    new_tbl->desc_count = desc_count;
    heapsort(new_descs, sizeof(struct arena_desc *), desc_count,
             arena_desc_cmp);

    rcu_assign_pointer(arena_global.desc_table, new_tbl);
    rcu_defer(&old_tbl->rcu_cb, arena_desc_table_free_cb);

out:
    spin_unlock(&arena_global.tbl_lock, irql);
    return err;
}

struct arena_desc *arena_desc_lookup(enum arena_strategy strat) {
    rcu_read_lock();

    struct arena_desc_table *tbl = rcu_dereference(arena_global.desc_table);

    struct arena_desc *found = bsearch(&strat, tbl, sizeof(struct arena_desc *),
                                       tbl->desc_count, arena_desc_cmp);

    rcu_read_unlock();

    return found;
}

void arena_global_init(void) {
    spinlock_init(&arena_global.tbl_lock);
    struct arena_desc_table *tbl = vnew(tbl, ALLOC_ZERO);

    rcu_assign_pointer(arena_global.desc_table, tbl);
}

static uint16_t size_for_id(struct arena_seg_desc *seg_descs, size_t seg_count,
                            uint16_t id) {
    for (size_t i = 0; i < seg_count; i++) {
        if (seg_descs[i].id == id)
            return seg_descs[i].size;
    }

    unreachable("invalid id");
}

/* Errors panic because the provider calls this */
struct arena *arena_create_full(struct arena_seg_desc *seg_descs,
                                size_t seg_count) {
    size_t seg_data_size_total = 0;
    struct arena_seg_desc *large_desc = NULL;
    BITMAP_DECLARE(seen_id_bitmap, ARENA_MAX_SEG) = {0};
    for (size_t i = 0; i < seg_count; i++) {
        enforce(seg_descs[i].size);

        /* It's fine if a large segment is smaller than ARENA_SEG_MAX_SIZE */
        if (seg_descs[i].large) {
            enforce(!large_desc, "more than one large segment exists");
            large_desc = &seg_descs[i];
        }

        if (seg_descs[i].size >= ARENA_SEG_MAX_SIZE)
            enforce(seg_descs[i].large, "large segment id %u must set .large",
                    seg_descs[i].id);

        enforce(!bitmap_test_and_set(seen_id_bitmap, seg_descs[i].id),
                "duplicate id %hu", seg_descs[i].id);

        seg_data_size_total += ALIGN_UP(seg_descs[i].size, ARENA_SEG_ALIGN);
    }

    size_t inmem_size_raw = seg_count * sizeof(struct arena_seg_inmem_desc);
    size_t inmem_size = ALIGN_UP(inmem_size_raw, ARENA_SEG_ALIGN);
    size_t seg_size_total = inmem_size + seg_data_size_total;
    size_t size_total = sizeof(struct arena) + seg_size_total;

    struct arena *arena = valloc(size_total, ALLOC_ZERO);
    if (!arena)
        return NULL;

#ifdef DEBUG_ARENA
    arena->seg_count = seg_count;
    arena->total_size = size_total;
#endif

    struct arena_seg_inmem_desc *inmem_descs = arena_get_inmem_descs(arena);

    if (large_desc) {
        inmem_descs[seg_count - 1].id = large_desc->id;

#ifdef DEBUG_ARENA
        inmem_descs[seg_count - 1].present = true;
        inmem_descs[seg_count - 1].type = large_desc->type;
        inmem_descs[seg_count - 1].size = large_desc->size;
#endif
    }

    size_t cursor = 0;
    for (size_t i = 0; i < seg_count; i++) {
        if (!seg_descs[i].large) {

#ifdef DEBUG_ARENA
            inmem_descs[cursor].present = true;
            inmem_descs[cursor].size = seg_descs[i].size;
            inmem_descs[cursor].type = seg_descs[i].type;
#endif

            inmem_descs[cursor].id = seg_descs[i].id;
            cursor++;
        }
    }

    size_t cc_maybe_unused cursor_offset = 0;
    for (uint16_t i = 0; i < seg_count; i++) {
        uint16_t size;

        if (i == 0) {
            size = inmem_size;
        } else {
            uint16_t id = inmem_descs[i - 1].id;
            size = ALIGN_UP(size_for_id(seg_descs, seg_count, id),
                            ARENA_SEG_ALIGN);
        }

        cursor_offset += size;

#ifdef DEBUG_ARENA
        inmem_descs[i].seg =
            (struct arena_seg *) &arena->payload[cursor_offset];
#endif

        inmem_descs[i].offset_bump = size;
    }

    arena_check_assert(arena);
    return arena;
}

size_t arena_seg_count(struct arena *a) {
    struct arena_seg_inmem_desc *descs = arena_get_inmem_descs(a);

#ifdef DEBUG_ARENA
    enforce(a->seg_count,
            "attempted to count segments of arena with no segments");
#endif

    size_t count = 0;
    uint16_t end = descs[0].offset_bump / sizeof(struct arena_seg_inmem_desc);
    for (uint16_t i = 0; i < end; i++) {
        struct arena_seg_inmem_desc *desc = &descs[i];
        if (desc->offset_bump)
            count++;
    }

    return count;
}

struct arena_seg arena_seg_lookup(struct arena *a, uint16_t seg_id) {
    struct arena_seg_inmem_desc *inmem_descs = arena_get_inmem_descs(a);

#ifdef DEBUG_ARENA
    bool found = false;
    for (uint16_t i = 0; i < a->seg_count; i++) {
        if (inmem_descs[i].id == seg_id)
            found = true;
    }

    enforce(found, "segment %u not found", seg_id);
#endif

    size_t cc_maybe_unused cursor = 0;
    for (int i = 0; i < ARENA_MAX_SEG; i++) {
        cursor += inmem_descs[i].offset_bump;
        if (inmem_descs[i].id == seg_id) {
            uint8_t *storage = &a->payload[cursor];
            return (struct arena_seg){.storage = storage,
                                      .id = inmem_descs[i].id};
        }
    }

    panic("segment %u not found, likely UB during traversal", seg_id);
}

struct arena_seg arena_seg_for_idx(struct arena *a, uint16_t idx) {
#ifdef DEBUG_ARENA
    enforce(idx < a->seg_count, "index %u out of bounds of %u", idx,
            a->seg_count);
#endif

    struct arena_seg_inmem_desc *inmem_descs = arena_get_inmem_descs(a);

    size_t cursor = 0;
    for (int i = 0; i < ARENA_MAX_SEG; i++) {
        cursor += inmem_descs[i].offset_bump;
        if (i == idx) {
            uint8_t *storage = &a->payload[cursor];
            return (struct arena_seg){.storage = storage,
                                      .id = inmem_descs[i].id};
        }
    }

    unreachable();
}

static struct arena_bucket *get_bucket(struct arena_dumpster *ad,
                                       arena_identity_t id) {
    uint32_t hash = hash_jenkins_qword(id, /* TODO: seed = */ 12345);
    size_t idx = hash % ad->bucket_count;

    struct arena_bucket *bkt = &ad->buckets[idx];
    return enforce(bkt);
}

static struct arena_bin *bucket_id_lookup(struct arena_bucket *bkt,
                                          arena_identity_t id) {
    struct arena_bin *entry, *leader = NULL;
    hlist_for_each_entry_rcu(entry, &bkt->bins, hlist_node) {
        if (entry->identity == id) {
            leader = entry;
            break;
        }
    }

    return leader;
}

/*
 * Memory arena buckets have physical and acting leaders. For every bucket,
 * we have an hlist that looks something like
 *
 * bucket
 * |
 * +-> hlist_head -> node -> node -> node -> NULL
 *
 * The nodes on this list are the "physical" leaders. However, the "physical"
 * leaders are merely an optimization, because the logical leaders are the
 * "acting" leaders.
 *
 * The reason we have this split at all is because any traversal of a bucket
 * leader list necessarily races with simultaneous deletion of arena bins,
 * i.e. marking the arena bin as dying and dropping its reference count.
 *
 * Such bins are NOT to be used any more, unless someone owned a reference
 * count prior to the start of the destruction.
 *
 * Thus, any traversal treats the leader as being the "first bin in the list
 * of the physical leader that we could successfully acquire a reference to".
 *
 * Thus, if a physical leader is alive, it is also the acting leader.
 * However, if a physical leader is dying/has died, but is still momentarily
 * visible on the list in the read-side critical section, as the RCU
 * free callback has not run, we know that the next-in-line is the acting
 * leader, which is where the arenas would get freed to in a dumpster.
 */
struct arena_bin *arena_dumpster_find_leader(struct arena_dumpster *ad,
                                             arena_identity_t id) {
    struct arena_bucket *bkt = get_bucket(ad, id);
    struct arena_bin *entry, **found = NULL;

    rcu_read_lock();

    struct arena_bin *first_leader = bucket_id_lookup(bkt, id);

    if (!first_leader)
        goto out;

    found = &first_leader;

    /* list_del_rcu prevents the middle links from getting
     * their own references wiped, so traversal under the
     * leader's list is fine */
    if (!arena_bin_get_rcu(first_leader)) {
        found = NULL;
        list_for_each_entry_rcu(entry, &first_leader->bin_list, bin_list) {
            if (arena_bin_get_rcu(entry)) {
                found = &entry;
                goto out;
            }
        }
    }

out:
    rcu_read_unlock();

    return found ? *found : NULL;
}

void arena_dumpster_add_bin(struct arena_dumpster *ad, struct arena_bin *ab) {
    struct arena_bucket *bkt = get_bucket(ad, ab->identity);
    struct mutex *mtx = &bkt->mutex;

    mutex_lock(mtx);

    struct arena_bin *leader = arena_dumpster_find_leader(ad, ab->identity);

    if (!leader) {
        INIT_LIST_HEAD(&ab->bin_list);
        hlist_add_head_rcu(&ab->hlist_node, &bkt->bins);
    } else {
        list_add_tail_rcu(&ab->bin_list, &leader->bin_list);
        ab->not_head_magic = ARENA_BIN_NOT_HEAD_MAGIC;
    }

    ab->dumpster = ad;
    mutex_unlock(mtx);

    if (leader)
        arena_bin_put(leader);
}

void arena_dumpster_delete_bin(struct arena_dumpster *ad,
                               struct arena_bin *ab) {
    struct arena_bucket *bkt = get_bucket(ad, ab->identity);
    struct mutex *mtx = &bkt->mutex;

    mutex_guard(mtx);

    struct arena_bin *leader = bucket_id_lookup(bkt, ab->identity);
    enforce(leader, "likely double delete");

    list_del_rcu(&ab->bin_list);
    if (leader == ab) {
        struct arena_bin *iter, *next = NULL;
        list_for_each_entry_rcu(iter, &leader->bin_list, bin_list) {
            /* This is the next physical leader */
            if (arena_bin_get_rcu(iter)) {
                next = iter;
                break;
            }
        }

        if (next)
            hlist_add_head_rcu(&next->hlist_node, &bkt->bins);
        hlist_del_rcu(&ab->hlist_node);
    }
}
