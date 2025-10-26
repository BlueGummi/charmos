#pragma once
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <misc/containerof.h>
#include <misc/list.h>
#include <misc/rbt.h>
#include <smp/domain.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>

/* Lock order:
 *
 * First acquire slab cache lock, then
 * per-slab lock. Freequeue has lockless
 * ringbuffer, but there is also a locked
 * singly linked list */

#define KMALLOC_PAGE_MAGIC 0xC0FFEE42

#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL

#define SLAB_OBJ_ALIGN 16u
#define SLAB_BITMAP_TEST(__bitmap, __idx) (__bitmap & __idx)

#define SLAB_CROSSFREE_RING_SIZE 64
#define SLAB_NONPAGEABLE_RESERVED_RATIO (1 / 16)
#define SLAB_DESTROY_HIGH_WATERMARK 4
#define SLAB_INTERLEAVE_STRIDE 1
#define SLAB_MAG_ENTRIES 32

#define SLAB_MIN_SIZE (sizeof(vaddr_t))
#define SLAB_MAX_SIZE (PAGE_SIZE / 4)

#define SLAB_BITMAP_BYTES_FOR(x) ((x + 7) / 8)
#define SLAB_BITMAP_SET(bm, mask) (bm |= (uint8_t) mask)
#define SLAB_BITMAP_UNSET(bm, mask) (bm &= (uint8_t) ~mask)

#define SLAB_ALIGN_UP(x, a) ALIGN_UP(x, a)
#define SLAB_OBJ_ALIGN_UP(x) SLAB_ALIGN_UP(x, SLAB_OBJ_ALIGN)

static const uint64_t slab_class_sizes[] = {
    SLAB_MIN_SIZE, 16, 32, 64, 96, 128, 192, 256, 512, SLAB_MAX_SIZE};

#define SLAB_CLASS_COUNT (sizeof(slab_class_sizes) / sizeof(*slab_class_sizes))

#define kmalloc_validate_params(size, flags, behavior)                         \
    do {                                                                       \
        kassert(alloc_flags_valid(flags));                                     \
        kassert(alloc_flag_behavior_verify(flags, behavior));                  \
        kassert((size) != 0);                                                  \
    } while (0)

/* This value determines the scale at which cores in a slab domain
 * will be weighted when they attempt to fill up their per-cpu
 * caches from free_queue elements.
 *
 * It is used to derive a "target amount of elements" to try to drain.
 *
 * The computation is as follows:
 *
 * target = fq_total_elems / (slab_domain_core_count / REFILL_PER_CORE_WEIGHT)
 *
 * Where (slab_domain_core_count / REFILL_PER_CORE_WEIGHT) is at least 1.
 *
 * Thus, as this number increases, the portion of all the free_queue elements
 * that will attempted to be flushed (the target) increases. */
#define SLAB_PERCPU_REFILL_PER_CORE_WEIGHT 2

enum slab_state {
    SLAB_FREE = 0,
    SLAB_PARTIAL = 1,
    SLAB_FULL = 2,
    SLAB_STANDARD_STATE_COUNT = 3,
    SLAB_IN_GC_LIST = 4,
};

struct slab {
    struct slab *self; /* We need this field because multi-page slabs
                        * will keep a pointer to the parent slab at
                        * the start of each page, and if we add this
                        * to the actual slab itself, we can branchlessly
                        * retrieve the slab for any given pointer.
                        *
                        * MUST be first element in structure  */

    /* Put commonly accessed fields up here to make cache happier */
    uint8_t *bitmap;
    vaddr_t mem;
    size_t used;
    struct slab_cache *parent_cache;

    enum slab_state state;
    struct spinlock lock;

    size_t pages;

    /* Sorted by gc_enqueue_time_ms */
    struct rbt_node rb;
    struct list_head list;

    time_t gc_enqueue_time_ms; /* When were we put on the GC list? */

    size_t recycle_count; /* How many times has this been
                           * recycled from the GC list? */
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab, lock);

#define slab_from_rbt_node(rb) (container_of(rb, struct slab, rb))
#define slab_from_list_node(ln) (container_of(ln, struct slab, list))
#define PAGE_NON_SLAB_SPACE (PAGE_SIZE - sizeof(struct slab))
#define slab_error(fmt, ...) k_info("SLAB", K_ERROR, fmt, ##__VA_ARGS__)

_Static_assert(offsetof(struct slab, self) == 0,
               "self pointer not at start of struct");

/* Just a simple stack */
struct slab_magazine {
    vaddr_t objs[SLAB_MAG_ENTRIES];
    uint8_t count;
    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_magazine, lock);

static inline bool slab_magazine_full(struct slab_magazine *mag) {
    return mag->count == SLAB_MAG_ENTRIES;
}

struct slab_percpu_cache {
    /* Magazines are always nonpageable */
    struct slab_magazine mag[SLAB_CLASS_COUNT];
};

struct slab_free_slot {
    atomic_uint_fast64_t seq;
    vaddr_t addr;
};

/* This is a cheeky little structure that
 * we embed into the data itself when
 * the free_queue ringbuffer is full.
 *
 * The idea is as follows:
 *
 * When we free an address, if the free_queue list
 * is empty, we set the free_queue list's head
 * address to the address that we are freeing,
 * as well as the free_queue list's tail.
 *
 * Upon subsequent frees, we read the
 * free_queue list's tail, and then
 * set its *next to the address we are
 * freeing, and set the tail to point
 * to the new address we are freeing, and
 * repeat this process.
 *
 * Because the minimum slab size is the pointer
 * size, we can guarantee that the pointer fits */
struct slab_free_queue_list_node {
    struct slab_free_queue_list_node *next;
};

struct slab_free_queue_list {
    struct slab_free_queue_list_node *head;
    struct slab_free_queue_list_node *tail;
    struct spinlock lock;
    size_t elements;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_free_queue_list, lock);

struct slab_free_queue {
    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;
    size_t capacity;
    struct slab_free_slot *slots;

    struct slab_free_queue_list list;
    atomic_size_t count;
};
#define SLAB_FREE_QUEUE_CAPACITY 2048 /* TODO: */
#define SLAB_FREE_QUEUE_GET_COUNT(fq) (atomic_load(&(fq)->count))
#define SLAB_FREE_QUEUE_INC_COUNT(fq) (atomic_fetch_add(&(fq)->count, 1))
#define SLAB_FREE_QUEUE_ADD_COUNT(fq, n) (atomic_fetch_add(&(fq)->count, n))
#define SLAB_FREE_QUEUE_SUB_COUNT(fq, n) (atomic_fetch_sub(&(fq)->count, n))
#define SLAB_FREE_QUEUE_DEC_COUNT(fq) (atomic_fetch_sub(&(fq)->count, 1))

enum slab_cache_type { SLAB_CACHE_TYPE_PAGEABLE, SLAB_CACHE_TYPE_NONPAGEABLE };

struct slab_cache {
    uint64_t obj_size;
    uint64_t objs_per_slab;
    size_t pages_per_slab;

    struct list_head slabs[SLAB_STANDARD_STATE_COUNT];
    atomic_size_t slabs_count[SLAB_STANDARD_STATE_COUNT];

    enum slab_cache_type type;

    struct slab_domain *parent_domain;

    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_cache, lock);

static inline size_t slab_cache_count_for(struct slab_cache *cache,
                                          enum slab_state state) {
    return atomic_load(&cache->slabs_count[state]);
}

struct slab_caches {
    struct slab_cache caches[SLAB_CLASS_COUNT];
};

struct slab_cache_ref {
    struct slab_domain *domain;
    struct slab_caches *caches; /* pointer to caches */
    enum slab_cache_type type;  /* pageable / nonpageable */
    uint8_t locality;           /* NUMA proximity, 0 = local */
};

struct slab_cache_zonelist {
    struct slab_cache_ref *entries;
    size_t count;
};

struct slab_gc {
    struct rbt rbt;
    struct spinlock lock;
    atomic_size_t num_elements;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_gc, lock);

struct slab_domain {
    /* Actual domain that this corresponds to */
    struct domain *domain;

    /* This domain's slab caches */
    struct slab_caches *local_nonpageable_cache;
    struct slab_caches *local_pageable_cache;

    /* Slab caches for each distance */
    struct slab_cache_zonelist nonpageable_zonelist;
    struct slab_cache_zonelist pageable_zonelist;

    /* Pointer to an array of pointers to per CPU single-slabs for each class */
    /* # CPUs determined by the domain struct */
    struct slab_percpu_cache **percpu_caches;

    /* Freequeue for remote frees */
    struct slab_free_queue free_queue;

    /* List of slabs that are reusable and can be
     * garbage collected safely/kept here */
    struct slab_gc slab_gc;

    struct daemon *daemon;
};

static inline struct domain_buddy *
slab_domain_buddy(struct slab_domain *domain) {
    return domain->domain->cores[0]->domain_buddy;
}

struct slab_page_hdr {
    uint32_t magic;
    uint32_t pages;
};

struct slab *slab_init(struct slab *slab, struct slab_cache *parent);
struct slab *slab_create(struct slab_domain *domain, struct slab_cache *cache);
void slab_destroy(struct slab *slab);
int32_t slab_size_to_index(size_t size);
void *slab_alloc(struct slab_cache *cache);
void slab_free_page_hdr(struct slab_page_hdr *hdr);
size_t slab_allocation_size(vaddr_t addr);
void slab_cache_init(struct slab_cache *cache, uint64_t obj_size);

/* Magazine */
bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj);
vaddr_t slab_magazine_pop(struct slab_magazine *mag);
vaddr_t slab_percpu_alloc(struct slab_domain *dom, size_t class_idx);
void slab_percpu_free(struct slab_domain *dom, size_t class_idx, vaddr_t obj);
void slab_free_addr_to_cache(void *addr);
void slab_domain_percpu_init(struct slab_domain *domain);

/* Freequeue */
void slab_free_queue_init(struct slab_free_queue *q, size_t capacity);
bool slab_free_queue_ringbuffer_enqueue(struct slab_free_queue *q,
                                        vaddr_t addr);
vaddr_t slab_free_queue_ringbuffer_dequeue(struct slab_free_queue *q);
void slab_free_queue_list_enqueue(struct slab_free_queue *q, vaddr_t addr);
vaddr_t slab_free_queue_list_dequeue(struct slab_free_queue *q);
vaddr_t slab_free_queue_dequeue(struct slab_free_queue *q);
size_t slab_free_queue_drain(struct slab_percpu_cache *cache,
                             struct slab_free_queue *queue, size_t target,
                             bool flush_to_cache);
size_t slab_free_queue_get_target_drain(struct slab_domain *domain);
void slab_free_queue_drain_limited(struct slab_percpu_cache *pc,
                                   struct slab_domain *dom);

/* Check */
bool slab_check(struct slab *slab);
#define slab_check_assert(slab) kassert(slab_check(slab))

/* GC */
struct slab *slab_reset(struct slab *slab);
void slab_gc_init(struct slab_gc *gc);
void slab_gc_enqueue(struct slab_domain *domain, struct slab *slab);
void slab_gc_dequeue(struct slab_domain *domain, struct slab *slab);
struct slab *slab_gc_get_newest(struct slab_domain *domain);
size_t slab_gc_num_slabs(struct slab_domain *domain);
size_t slab_gc_flush_up_to(struct slab_domain *domain, size_t max);
size_t slab_gc_flush_full(struct slab_domain *domain);

static inline struct page *slab_get_backing_page(struct slab *slab) {
    return page_for_pfn(PAGE_TO_PFN(vmm_get_phys((vaddr_t) slab)));
}

static inline struct slab_page_hdr *slab_page_hdr_for_addr(void *ptr) {
    return (struct slab_page_hdr *) PAGE_ALIGN_DOWN(ptr);
}

static inline size_t slab_object_count(struct slab *slab) {
    return slab->parent_cache->objs_per_slab;
}

static inline size_t slab_object_size(struct slab *slab) {
    return slab->parent_cache->obj_size;
}

/* The parent slab object has a pointer
 * to itself at the start of the structure,
 * and following pages all contain a backpointer
 * to the slab at the start of every page */
static inline struct slab *slab_for_ptr(void *ptr) {
    return *(struct slab **) PAGE_ALIGN_DOWN(ptr);
}

static inline struct slab_domain *slab_domain_local(void) {
    return smp_core()->slab_domain;
}

static inline struct slab_percpu_cache *slab_percpu_cache_local(void) {
    return slab_domain_local()->percpu_caches[smp_core_id()];
}

static inline void slab_list_del(struct slab *slab) {
    if (list_empty(&slab->list))
        return;

    enum slab_state state = slab->state;
    list_del(&slab->list);

    if (state != SLAB_IN_GC_LIST)
        atomic_fetch_sub(&slab->parent_cache->slabs_count[state], 1);
}

static inline void slab_list_add(struct slab_cache *cache, struct slab *slab) {
    enum slab_state state = slab->state;
    list_add(&slab->list, &cache->slabs[state]);
    atomic_fetch_add(&slab->parent_cache->slabs_count[state], 1);
}

static inline void slab_move(struct slab_cache *c, struct slab *slab,
                             enum slab_state new) {
    kassert(spinlock_held(&c->lock));
    slab_list_del(slab);

    slab->state = new;

    slab_list_add(c, slab);
}

static inline bool alloc_behavior_can_gc(enum alloc_behavior b) {
    return alloc_behavior_may_fault(b);
}

extern struct vas_space *slab_vas;
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
