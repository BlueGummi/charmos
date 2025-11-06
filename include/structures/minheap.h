#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>
#define MINHEAP_INIT_CAP 32
#define MINHEAP_INDEX_INVALID ((uint32_t) -1)

#define minheap_for_each(heap, node_ptr)                                       \
    for (uint32_t __i = 0;                                                     \
         (node_ptr = ((heap)->nodes[__i]), __i < (heap)->size); __i++)

struct minheap_node {
    struct spinlock lock;
    atomic_uint_fast64_t key;
    atomic_uint_fast32_t index;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(minheap_node, lock);

struct minheap {
    struct minheap_node **nodes;
    atomic_uint_fast32_t capacity;
    atomic_uint_fast32_t size;
};

struct minheap *minheap_create(void);
void minheap_insert(struct minheap *heap, struct minheap_node *node,
                    uint64_t key);
void minheap_remove(struct minheap *heap, struct minheap_node *node);
void minheap_expand(struct minheap *heap, uint32_t new_size);

#define MINHEAP_SIZE(mh) (atomic_load(&mh->size))
#define MINHEAP_CAPACITY(mh) (atomic_load(&mh->capacity))
#define MINHEAP_NODE_KEY(mhn) (atomic_load(&mhn->key))
#define MINHEAP_NODE_INDEX(mhn) (atomic_load(&mhn->index))

#define MINHEAP_SET_SIZE(mh, n) (atomic_store(&mh->size, n))
#define MINHEAP_SET_CAPACITY(mh, n) (atomic_store(&mh->capacity, n))
#define MINHEAP_NODE_SET_KEY(mhn, n) (atomic_store(&mhn->key, n))
#define MINHEAP_NODE_SET_INDEX(mhn, n) (atomic_store(&mhn->index, n))

#define MINHEAP_NODE_INVALID(mhn)                                              \
    (MINHEAP_NODE_INDEX(mhn) == MINHEAP_INDEX_INVALID)
#define MINHEAP_MARK_NODE_INVALID(mhn)                                         \
    (MINHEAP_NODE_SET_INDEX(mhn, MINHEAP_INDEX_INVALID))

static inline struct minheap_node *minheap_peek(struct minheap *heap) {
    return MINHEAP_SIZE(heap) == 0 ? NULL : heap->nodes[0];
}

struct minheap_node *minheap_pop(struct minheap *heap);

static inline bool minheap_node_valid(struct minheap_node *node) {
    enum irql irql = minheap_node_lock_irq_disable(node);
    bool valid = MINHEAP_NODE_INDEX(node) != MINHEAP_INDEX_INVALID;
    minheap_node_unlock(node, irql);
    return valid;
}
