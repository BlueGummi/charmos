#include <compiler/intrinsic.h>
#include <console/panic.h>
#include <enforce.h>
#include <math/align.h>
#include <math/bit.h>
#include <mem/address_range.h>
#include <mem/alloc.h>
#include <mem/hhdm.h>
#include <mem/pmm.h>
#include <smp/core.h>
#include <string.h>

#include "vas_internal.h"

static cpu_id_t vas_cpu_id(void) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return 0;
    return smp_id(TOPC_IRQL);
}

static enum irql vas_enter(void) TSA_ACQUIRES(IRQL_RAISED) TSA_NO_ANALYSIS {
    if (global.current_bootstage >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic("vas: hardware interrupt context");
    return irql_raise(IRQL_DISPATCH_LEVEL);
}

static const size_t mag_sizes[VAS_MAG_CLASSES] = {
    PAGE_SIZE,
    5 * PAGE_SIZE,
    17 * PAGE_SIZE,
    PAGE_2MB,
};
static const uint32_t mag_capacities[VAS_MAG_CLASSES] = {VAS_MAG_CAPACITY, 8, 8,
                                                         2};

static uint32_t mag_class(size_t size) {
    for (uint32_t i = 0; i < VAS_MAG_CLASSES; i++)
        if (mag_sizes[i] == size)
            return i;
    return VAS_MAG_CLASSES;
}

static bool magazines_enabled(struct vas *vas) {
#ifdef TEST_ENABLED
    if (vas->magazines_disabled)
        return false;
#else
    cc_unused(vas);
#endif
    /* IRQL is a no-op during bootstrap */
    return global.current_bootstage >= BOOTSTAGE_LATE;
}

static bool magazine_claim(struct vas_mag_slot *slot, uintptr_t token) {
    uintptr_t claimed = (token & ~VAS_MAG_STATE_MASK) | VAS_MAG_CLAIMED;
    return atomic_cas_strong(&slot->token, &token, claimed, mo_acquire, mo_rlx);
}

static void magazine_publish(struct vas_mag_slot *slot,
                             enum vas_mag_state state) {
    struct vas_segment *seg = slot->segment;
    vaddr_t addr = seg->start;
    atomic_store_release(&seg->type,
                         state == VAS_MAG_LIVE ? VAS_SEG_BUSY : VAS_SEG_CACHED);
    atomic_store_release(&slot->token, vas_token_make(addr, state));
}

static vaddr_t magazine_alloc(struct vas_arena *arena, uint32_t cls,
                              size_t align) {
    struct vas_magazine *mag = &arena->magazines[cls];
    for (uint32_t n = 0; n < mag_capacities[cls]; n++) {
        uint32_t i = (mag->recent + n) % mag_capacities[cls];
        struct vas_mag_slot *slot = &mag->slots[i];
        uintptr_t token = atomic_load_rlx(&slot->token);

        vaddr_t addr = token & ~VAS_MAG_STATE_MASK;

        bool uncached = (token & VAS_MAG_STATE_MASK) != VAS_MAG_CACHED;
        bool unaligned = !IS_ALIGNED(addr, align);

        if (uncached || unaligned)
            continue;

        if (!magazine_claim(slot, token))
            continue;

        magazine_publish(slot, VAS_MAG_LIVE);

#ifdef TEST_ENABLED
        arena->mag_alloc_hits++;
#endif

        return addr;
    }
    return 0;
}

static bool magazine_free(struct vas_arena *arena, uint32_t cls, vaddr_t addr) {
    if (!IS_PAGE_ALIGNED(addr))
        return false;
    struct vas_magazine *mag = &arena->magazines[cls];
    for (uint32_t i = 0; i < mag_capacities[cls]; i++) {
        struct vas_mag_slot *slot = &mag->slots[i];
        uintptr_t token = vas_token_make(addr, VAS_MAG_LIVE);

        if (atomic_load_rlx(&slot->token) != token ||
            !magazine_claim(slot, token))
            continue;

        magazine_publish(slot, VAS_MAG_CACHED);
        mag->recent = i;
#ifdef TEST_ENABLED
        arena->mag_free_hits++;
#endif
        return true;
    }
    return false;
}

static size_t segment_key(struct rbt_node *node) {
    return rbt_entry(node, struct vas_segment, node)->start;
}

static int32_t segment_cmp(const struct rbt_node *a, const struct rbt_node *b) {
    vaddr_t l = segment_key((struct rbt_node *) a);
    vaddr_t r = segment_key((struct rbt_node *) b);
    return (l > r) - (l < r);
}

static uint32_t size_to_bin(size_t size) {
    enforce(size);
    return 63U - ci_clzll(size);
}

static void bin_insert(struct vas_arena *arena, struct vas_segment *seg) {
    uint32_t bin = size_to_bin(seg->length);
    list_add(&seg->bin_node, &arena->free_bins[bin]);
    arena->bin_mask = BIT_SET(arena->bin_mask, bin);
}

static void bin_remove(struct vas_arena *arena, struct vas_segment *seg) {
    uint32_t bin = size_to_bin(seg->length);
    list_del_init(&seg->bin_node);
    if (list_empty(&arena->free_bins[bin]))
        arena->bin_mask = BIT_CLEAR(arena->bin_mask, bin);
}

static struct vas_segment *segment_alloc(struct vas_arena *arena) {
#ifdef TEST_ENABLED
    if (arena->tag_alloc_budget == 0)
        return NULL;
    if (arena->tag_alloc_budget > 0)
        arena->tag_alloc_budget--;
#endif
    struct vas_segment *seg = fixed_size_alloc(&arena->fsr);
    if (seg) {
        memset(seg, 0, sizeof(*seg));
        atomic_init(&seg->type, VAS_SEG_FREE);
        rbt_init_node(&seg->node);
        INIT_LIST_HEAD(&seg->seg_node);
        INIT_LIST_HEAD(&seg->bin_node);
    }
    return seg;
}

static void segment_insert_after(struct vas_arena *arena,
                                 struct vas_segment *seg,
                                 struct list_head *prev) {
    rbt_init_node(&seg->node);
    rbt_insert(&arena->tree, &seg->node);
    list_add(&seg->seg_node, prev);
}

static void segment_delete(struct vas_arena *arena, struct vas_segment *seg) {
    rbt_delete(&arena->tree, &seg->node);
    list_del(&seg->seg_node);
    fixed_size_free(&arena->fsr, seg);
}

static struct vas_segment *segment_find(struct vas_arena *arena, vaddr_t addr) {
    struct rbt_node *node = arena->tree.root;
    while (node) {
        struct vas_segment *seg = rbt_entry(node, struct vas_segment, node);
        if (addr < seg->start)
            node = node->left;
        else if (addr - seg->start >= seg->length)
            node = node->right;
        else
            return seg;
    }
    return NULL;
}

static void arena_init(struct vas_arena *arena, bool is_global) {
    /* For lock classes, the order is global -> local */
    if (is_global) {
        spinlock_init(&arena->lock);
    } else {
        spinlock_init(&arena->lock);
    }
    rbt_init(&arena->tree, segment_key, segment_cmp);
    INIT_LIST_HEAD(&arena->all_segs);
    for (uint32_t i = 0; i < VAS_BIN_COUNT; i++)
        INIT_LIST_HEAD(&arena->free_bins[i]);
    arena->bin_mask = 0;
    arena->total_free = 0;
    for (uint32_t cls = 0; cls < VAS_MAG_CLASSES; cls++) {
        arena->magazines[cls].recent = 0;
        for (uint32_t i = 0; i < VAS_MAG_CAPACITY; i++)
            atomic_init(&arena->magazines[cls].slots[i].token, 0);
    }
    struct fixed_size_range_attributes attrs = {
        .obj_size = sizeof(struct vas_segment),
        .obj_align = _Alignof(struct vas_segment),
        .bootstrap_mode = true,
    };
    fixed_size_range_init(&arena->fsr, &attrs);
#ifdef TEST_ENABLED
    arena->tag_alloc_budget = -1;
#endif
}

static struct vas_segment *arena_alloc(struct vas_arena *arena, size_t size,
                                       size_t align, bool *metadata_failed) {
    uint64_t mask = arena->bin_mask & (UINT64_MAX << size_to_bin(size));
    while (mask) {
        uint32_t bin = ci_ctzll(mask);
        struct list_head *pos;
        list_for_each(pos, &arena->free_bins[bin]) {
            struct vas_segment *seg =
                list_entry(pos, struct vas_segment, bin_node);
            size_t pad = (-seg->start) & (align - 1);
            if (pad > seg->length || size > seg->length - pad)
                continue;

            size_t tail = seg->length - pad - size;

            struct vas_segment *left = pad ? segment_alloc(arena) : NULL;
            if (pad && !left) {
                *metadata_failed = true;
                return NULL;
            }

            struct vas_segment *right = tail ? segment_alloc(arena) : NULL;
            if (tail && !right) {
                if (left)
                    fixed_size_free(&arena->fsr, left);
                *metadata_failed = true;
                return NULL;
            }

            /* No fallible operations after this */
            bin_remove(arena, seg);
            rbt_delete(&arena->tree, &seg->node);
            if (left) {
                left->start = seg->start;
                left->length = pad;
                left->span_start = seg->span_start;
                segment_insert_after(arena, left, seg->seg_node.prev);
                bin_insert(arena, left);
            }
            seg->start += pad;
            seg->length = size;
            atomic_store_rlx(&seg->type, VAS_SEG_BUSY);
            rbt_init_node(&seg->node);
            rbt_insert(&arena->tree, &seg->node);
            if (right) {
                right->start = seg->start + size;
                right->length = tail;
                right->span_start = seg->span_start;
                segment_insert_after(arena, right, &seg->seg_node);
                bin_insert(arena, right);
            }
            arena->total_free -= size;
            return seg;
        }
        mask &= mask - 1;
    }
    return NULL;
}

static bool segments_mergeable(struct vas_segment *left,
                               struct vas_segment *right) {
    return atomic_load_rlx(&left->type) == VAS_SEG_FREE &&
           atomic_load_rlx(&right->type) == VAS_SEG_FREE &&
           left->span_start == right->span_start &&
           left->length == right->start - left->start;
}

/* The tag already exists */
static void arena_release(struct vas_arena *arena, struct vas_segment *seg) {
    arena->total_free += seg->length;
    atomic_store_rlx(&seg->type, VAS_SEG_FREE);
    if (seg->seg_node.prev != &arena->all_segs) {
        struct vas_segment *prev =
            list_entry(seg->seg_node.prev, struct vas_segment, seg_node);
        if (segments_mergeable(prev, seg)) {
            bin_remove(arena, prev);
            prev->length += seg->length;
            segment_delete(arena, seg);
            seg = prev;
        }
    }
    if (seg->seg_node.next != &arena->all_segs) {
        struct vas_segment *next =
            list_entry(seg->seg_node.next, struct vas_segment, seg_node);
        if (segments_mergeable(seg, next)) {
            bin_remove(arena, next);
            seg->length += next->length;
            segment_delete(arena, next);
        }
    }
    bin_insert(arena, seg);
}

/* Arena lock is held */
static bool magazine_enroll(struct vas *vas, struct vas_arena *arena,
                            struct vas_segment *seg, uint32_t cls,
                            enum vas_mag_state state) {
    if (!IS_ALIGNED(seg->start, PAGE_SIZE))
        return false;

    struct vas_mag_slot *slot = NULL;
    for (uint32_t i = 0; i < mag_capacities[cls]; i++) {
        if (!atomic_load_rlx(&arena->magazines[cls].slots[i].token)) {
            slot = &arena->magazines[cls].slots[i];
            break;
        }
    }
    if (!slot)
        return false;
    size_t bytes = atomic_load_rlx(&vas->mag_reserved_bytes);
    do {
        if (seg->length > VAS_MAG_BYTE_LIMIT - bytes)
            return false;
    } while (!atomic_cas_weak(&vas->mag_reserved_bytes, &bytes,
                              bytes + seg->length, mo_rlx, mo_rlx));
    seg->mag_slot = slot;
    slot->segment = seg;
    magazine_publish(slot, state);
    return true;
}

/* Best-effort refill */
static void magazine_refill(struct vas *vas, struct vas_arena *arena,
                            struct vas_segment *allocated, size_t align) {
    uint32_t cls = mag_class(allocated->length);
    if (cls == VAS_MAG_CLASSES || !magazines_enabled(vas) ||
        !magazine_enroll(vas, arena, allocated, cls, VAS_MAG_LIVE))
        return;
    for (uint32_t i = 1; i < VAS_MAG_REFILL && i < mag_capacities[cls]; i++) {
        bool failed = false;
        struct vas_segment *seg =
            arena_alloc(arena, mag_sizes[cls], align, &failed);
        if (!seg)
            break;
        if (!magazine_enroll(vas, arena, seg, cls, VAS_MAG_CACHED)) {
            arena_release(arena, seg);
            break;
        }
    }
}

/* Arena lock and a CLAIMED token held */
static void magazine_release(struct vas *vas, struct vas_arena *arena,
                             struct vas_mag_slot *slot) {
    struct vas_segment *seg = slot->segment;
    atomic_fetch_sub_rlx(&vas->mag_reserved_bytes, seg->length);
    seg->mag_slot = NULL;
    slot->segment = NULL;
    arena_release(arena, seg);
    atomic_store_release(&slot->token, 0);
}

static void magazine_drain(struct vas *vas, struct vas_arena *arena) {
    for (uint32_t cls = 0; cls < VAS_MAG_CLASSES; cls++) {
        for (uint32_t i = 0; i < mag_capacities[cls]; i++) {
            struct vas_mag_slot *slot = &arena->magazines[cls].slots[i];
            uintptr_t token = atomic_load_rlx(&slot->token);
            if ((token & VAS_MAG_STATE_MASK) != VAS_MAG_CACHED)
                continue;
            if (magazine_claim(slot, token))
                magazine_release(vas, arena, slot);
        }
    }
}

static size_t owner_index(struct vas *vas, vaddr_t addr) {
    return (addr - vas->map_base) >> VAS_CHUNK_SHIFT;
}

/* Global lock held */
static void reclaim_locked(struct vas *vas) {
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        struct vas_arena *local = &vas->local[cpu];

        enum irql irql = spin_lock(&local->lock);
        magazine_drain(vas, local);
        struct list_head *pos, *next;

        list_for_each_safe(pos, next, &local->all_segs) {
            struct vas_segment *seg =
                list_entry(pos, struct vas_segment, seg_node);

            if (atomic_load_rlx(&seg->type) != VAS_SEG_FREE ||
                seg->start != seg->span_start || seg->length != VAS_CHUNK_SIZE)
                continue;

            vaddr_t start = seg->start;
            struct vas_segment *parent = segment_find(&vas->global, start);
            enforce(parent && parent->start == start &&
                    parent->length == VAS_CHUNK_SIZE &&
                    atomic_load_rlx(&parent->type) == VAS_SEG_IMPORTED);

            bin_remove(local, seg);
            local->total_free -= seg->length;
            segment_delete(local, seg);
            arena_release(&vas->global, parent);
            atomic_store_release(&vas->chunk_owner[owner_index(vas, start)],
                                 VAS_OWNER_GLOBAL);
        }
        spin_unlock(&local->lock, irql);
    }
}

/* Global lock held, build allocation and then publish owner */
static vaddr_t import_alloc(struct vas *vas, uint32_t cpu, size_t size,
                            size_t align, bool *metadata_failed) {
    struct vas_arena *local = &vas->local[cpu];
    enum irql irql = spin_lock(&local->lock);
    struct vas_segment *seg = segment_alloc(local);
    if (!seg) {
        *metadata_failed = true;
        spin_unlock(&local->lock, irql);
        return 0;
    }
    struct vas_segment *parent = arena_alloc(&vas->global, VAS_CHUNK_SIZE,
                                             VAS_CHUNK_SIZE, metadata_failed);
    if (!parent) {
        fixed_size_free(&local->fsr, seg);
        spin_unlock(&local->lock, irql);
        return 0;
    }

    atomic_store_rlx(&parent->type, VAS_SEG_IMPORTED);
    seg->start = seg->span_start = parent->start;
    seg->length = VAS_CHUNK_SIZE;

    struct rbt_node *prev = rbt_find_predecessor(&local->tree, seg->start);
    struct list_head *before =
        prev ? &rbt_entry(prev, struct vas_segment, node)->seg_node
             : &local->all_segs;

    segment_insert_after(local, seg, before);
    bin_insert(local, seg);

    local->total_free += VAS_CHUNK_SIZE;

    struct vas_segment *allocated =
        arena_alloc(local, size, align, metadata_failed);

    if (!allocated) {
        bin_remove(local, seg);
        local->total_free -= VAS_CHUNK_SIZE;
        segment_delete(local, seg);
        arena_release(&vas->global, parent);
        spin_unlock(&local->lock, irql);
        return 0;
    }

    atomic_store_release(&vas->chunk_owner[owner_index(vas, parent->start)],
                         cpu);
    vaddr_t result = allocated->start;
    magazine_refill(vas, local, allocated, align);
    spin_unlock(&local->lock, irql);
    return result;
}

vaddr_t vas_alloc(struct vas *vas, size_t size, size_t align) {
    if (!vas || !size || !IS_POW2(align) || size > vas->limit - vas->base)
        return 0;

    enum irql outer = vas_enter();
    cpu_id_t cpu = vas_cpu_id();

    bool metadata_failed = false;
    bool local_request = size <= VAS_CHUNK_SIZE && align <= VAS_CHUNK_SIZE;
    vaddr_t result = 0;
    uint32_t cls = mag_class(size);

#ifdef TEST_ENABLED
    vas->local[cpu].requests[cls]++;
    vas->local[cpu].alignments[size_to_bin(align)]++;
#endif
    if (cls < VAS_MAG_CLASSES && magazines_enabled(vas)) {
        result = magazine_alloc(&vas->local[cpu], cls, align);
        if (result)
            goto out;
    }

    if (local_request) {
        struct vas_arena *local = &vas->local[cpu];
        enum irql irql = spin_lock(&local->lock);
        struct vas_segment *seg =
            arena_alloc(local, size, align, &metadata_failed);

        if (seg) {
            result = seg->start;
            magazine_refill(vas, local, seg, align);
        }

        spin_unlock(&local->lock, irql);
        if (result || metadata_failed)
            goto out;
    }

    enum irql irql = spin_lock(&vas->global.lock);
    for (uint32_t attempt = 0; attempt < 2; attempt++) {
        if (local_request)
            result = import_alloc(vas, cpu, size, align, &metadata_failed);

        if (!result && !metadata_failed) {
            /* Includes partial edge buckets and aligned small fits */
            struct vas_segment *seg =
                arena_alloc(&vas->global, size, align, &metadata_failed);

            if (seg)
                result = seg->start;
        }

        if (result || metadata_failed || attempt == 1)
            break;

        reclaim_locked(vas);
    }

    spin_unlock(&vas->global.lock, irql);

out:
    irql_lower(outer);
    return result;
}

/* Directory is never freed while operations are in flight, recheck
 * under the lock because reclamation could've moved the chunk */
static enum irql vas_lock_owner(struct vas *vas, vaddr_t addr,
                                struct vas_arena **out_arena)
    TSA_ACQUIRES_SPIN(&(*out_arena)->lock) TSA_NO_ANALYSIS {
    size_t index = owner_index(vas, addr);
    while (true) {
        cpu_id_t owner = atomic_load_acq(&vas->chunk_owner[index]);
        enforce(owner == VAS_OWNER_GLOBAL || owner < global.core_count);
        struct vas_arena *arena =
            owner == VAS_OWNER_GLOBAL ? &vas->global : &vas->local[owner];
        enum irql irql = spin_lock(&arena->lock);
        if (atomic_load_acq(&vas->chunk_owner[index]) == owner) {
            *out_arena = arena;
            return irql;
        }
        spin_unlock(&arena->lock, irql);
    }
}

bool vas_vaddr_in_vas(struct vas *vas, vaddr_t addr) {
    return vas && addr >= vas->base && addr < vas->limit;
}

void vas_free(struct vas *vas, vaddr_t addr, size_t size) {
    if (!vas_vaddr_in_vas(vas, addr) || !size || size > vas->limit - addr)
        panic("vas_free: invalid extent %p+%zu", (void *) addr, size);

    enum irql outer = vas_enter();
    uint32_t cls = mag_class(size);

    if (cls < VAS_MAG_CLASSES && magazines_enabled(vas) &&
        magazine_free(&vas->local[vas_cpu_id()], cls, addr)) {
        irql_lower(outer);
        return;
    }

    struct vas_arena *arena;
    enum irql irql = vas_lock_owner(vas, addr, &arena);
    struct vas_segment *seg = segment_find(arena, addr);

    if (!seg || atomic_load_rlx(&seg->type) != VAS_SEG_BUSY ||
        seg->start != addr || seg->length != size)
        panic("vas_free: invalid allocation/double free at %p+%zu",
              (void *) addr, size);

    if (seg->mag_slot) {
        struct vas_mag_slot *slot = seg->mag_slot;
        uintptr_t token = vas_token_make(addr, VAS_MAG_LIVE);
        if (!magazine_claim(slot, token))
            panic("vas_free: concurrent/duplicate magazine free at %p",
                  (void *) addr);

        magazine_release(vas, arena, slot);
    } else {
        arena_release(arena, seg);
    }

    spin_unlock(&arena->lock, irql);
    irql_lower(outer);
}

bool vas_vaddr_is_allocated(struct vas *vas, vaddr_t addr) {
    if (!vas_vaddr_in_vas(vas, addr))
        return false;

    enum irql outer = vas_enter();

    struct vas_arena *arena;
    enum irql irql = vas_lock_owner(vas, addr, &arena);
    struct vas_segment *seg = segment_find(arena, addr);
    bool allocated = seg && atomic_load_rlx(&seg->type) == VAS_SEG_BUSY;
    spin_unlock(&arena->lock, irql);

    irql_lower(outer);
    return allocated;
}

void vas_reclaim(struct vas *vas) {
    enum irql irql = spin_lock(&vas->global.lock);
    reclaim_locked(vas);
    spin_unlock(&vas->global.lock, irql);
}

void vas_reclaim_freelist_pages(struct vas_arena *arena) {
    fixed_size_reclaim_freelist_pages(&arena->fsr);
}

static void arena_destroy(struct vas_arena *arena) {
    struct list_head *pos, *next;

    list_for_each_safe(pos, next, &arena->all_segs) {
        struct vas_segment *seg = list_entry(pos, struct vas_segment, seg_node);
        if (atomic_load_rlx(&seg->type) == VAS_SEG_FREE)
            bin_remove(arena, seg);

        segment_delete(arena, seg);
    }

    vas_reclaim_freelist_pages(arena);
}

static void space_storage_free(struct vas *vas) {
    if (vas->bootstrap_pages) {
        pmm_free_pages(hhdm_ptr_to_paddr(vas), vas->bootstrap_pages);
    } else {
        vfree(vas);
    }
}

bool vas_destroy(struct vas *vas) {
    if (!vas)
        return true;

    vas_reclaim(vas);
    if (vas->global.total_free != vas->limit - vas->base)
        return false;

    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++)
        arena_destroy(&vas->local[cpu]);

    arena_destroy(&vas->global);
    space_storage_free(vas);
    return true;
}

static struct vas *space_create(vaddr_t base, vaddr_t limit, bool bootstrap) {
    enforce(global.core_count && global.core_count < VAS_OWNER_GLOBAL);
    if (!base || limit <= base)
        return NULL;

    vaddr_t map_base = ALIGN_DOWN(base, VAS_CHUNK_SIZE);
    size_t owners = ((limit - 1 - map_base) >> VAS_CHUNK_SHIFT) + 1;
    size_t arena_bytes = global.core_count * sizeof(struct vas_arena);
    size_t overhead = sizeof(struct vas) + arena_bytes;

    if (owners > (SIZE_MAX - overhead) / sizeof(atomic(cpu_id_t)))
        return NULL;

    size_t bytes = overhead + owners * sizeof(atomic(cpu_id_t));
    if (bytes > SIZE_MAX - (PAGE_SIZE - 1))
        return NULL;

    size_t pages = PAGES_NEEDED_FOR(bytes);
    struct vas *vas;
    if (bootstrap) {
        paddr_t phys = pmm_alloc_pages(pages);
        if (!phys)
            return NULL;

        vas = hhdm_paddr_to_ptr(phys);
        memset(vas, 0, pages * PAGE_SIZE);
    } else {
        vas = valloc(bytes, ALLOC_ZERO,
                     .behavior =
                         ALLOC_BEHAVIOR_NORMAL | ALLOC_BEHAVIOR_FLAG_MINIMAL);
        if (!vas)
            return NULL;
    }

    vas->base = base;
    vas->limit = limit;
    vas->map_base = map_base;
    vas->owner_count = owners;
    vas->bootstrap_pages = bootstrap ? pages : 0;
    vas->local = (void *) ((uint8_t *) vas + sizeof(*vas));
    vas->chunk_owner = (void *) ((uint8_t *) vas + overhead);

    atomic_init(&vas->mag_reserved_bytes, 0);
    for (size_t i = 0; i < owners; i++)
        atomic_init(&vas->chunk_owner[i], VAS_OWNER_GLOBAL);

    arena_init(&vas->global, true);
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++)
        arena_init(&vas->local[cpu], false);

    struct vas_segment *seg = segment_alloc(&vas->global);
    if (!seg) {
        space_storage_free(vas);
        return NULL;
    }
    seg->start = base;
    seg->length = limit - base;
    segment_insert_after(&vas->global, seg, &vas->global.all_segs);
    bin_insert(&vas->global, seg);
    vas->global.total_free = seg->length;
    return vas;
}

struct vas *vas_bootstrap(vaddr_t base, vaddr_t limit) {
    return space_create(base, limit, true);
}

struct vas *vas_create(vaddr_t base, vaddr_t limit) {
    return space_create(base, limit, false);
}

static struct vas *space_from(struct address_range *ar, bool bootstrap) {
    if (!ar || ar->size > UINTPTR_MAX - ar->base)
        return NULL;

    return space_create(ar->base, ar->base + ar->size, bootstrap);
}

struct vas *vas_from(struct address_range *ar) {
    return space_from(ar, false);
}

struct vas *vas_bootstrap_from(struct address_range *ar) {
    return space_from(ar, true);
}

static size_t mapped_extent_size(uintptr_t addr, size_t len) {
    size_t offset = addr & (PAGE_SIZE - 1);
    if (!len || len - 1 > UINTPTR_MAX - addr || len > SIZE_MAX - offset ||
        len + offset > SIZE_MAX - (PAGE_SIZE - 1))
        return 0;
    return ALIGN_UP(len + offset, PAGE_SIZE);
}

void *vas_map_full(struct vas *vas, paddr_t paddr, size_t len,
                   struct vmm_request rq) {
    size_t size = mapped_extent_size(paddr, len);
    if (!size)
        return NULL;

    vaddr_t vaddr = vas_alloc(vas, size, PAGE_SIZE);
    if (!vaddr)
        return NULL;

    void *ret = vmm_map_full(paddr, vaddr, len, rq);
    if (!ret)
        vas_free(vas, vaddr, size);
    return ret;
}

void vas_unmap(struct vas *vas, void *vaddr, size_t len) {
    size_t size = mapped_extent_size((vaddr_t) vaddr, len);
    if (!size)
        panic("vas_unmap: invalid length %zu", len);

    vaddr_t base = PAGE_ALIGN_DOWN(vaddr);
    vmm_unmap((void *) base, size);
    vas_free(vas, base, size);
}

static void arena_dump(struct vas_arena *arena) {
    enum irql irql = spin_lock(&arena->lock);
    struct list_head *pos;
    list_for_each(pos, &arena->all_segs) {
        struct vas_segment *seg = list_entry(pos, struct vas_segment, seg_node);
        enum vas_segment_type type = atomic_load_rlx(&seg->type);
        printf("    %p .. %p len=%zu %s\n", (void *) seg->start,
               (void *) (seg->start + seg->length), seg->length,
               type == VAS_SEG_CACHED     ? "cached"
               : type == VAS_SEG_FREE     ? "free"
               : type == VAS_SEG_IMPORTED ? "imported"
                                          : "busy");
    }
    printf("    total_free=%zu\n", arena->total_free);
    spin_unlock(&arena->lock, irql);
}

void vas_space_dump(struct vas *vas) {
    printf("vas %p: %p .. %p chunk_size=%zu\n", vas, (void *) vas->base,
           (void *) vas->limit, (size_t) VAS_CHUNK_SIZE);
    printf("  global:\n");
    arena_dump(&vas->global);
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        printf("  cpu[%zu]:\n", cpu);
        arena_dump(&vas->local[cpu]);
    }
}
