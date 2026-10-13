#include <console/panic.h>
#include <mem/alloc.h>
#include <string.h>
#include <structures/minheap.h>
#include <sync/spinlock.h>

static void minheap_swap(struct minheap *heap, uint32_t a, uint32_t b) {
    struct minheap_node *tmp = heap->nodes[a];
    heap->nodes[a] = heap->nodes[b];
    heap->nodes[b] = tmp;

    MINHEAP_NODE_SET_INDEX(heap->nodes[a], a);
    MINHEAP_NODE_SET_INDEX(heap->nodes[b], b);
}

static void minheap_sift_up(struct minheap *heap, uint32_t idx) {
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        if (MINHEAP_NODE_KEY(heap->nodes[idx]) >=
            MINHEAP_NODE_KEY(heap->nodes[parent]))
            break;

        minheap_swap(heap, idx, parent);
        idx = parent;
    }
}

static void minheap_sift_down(struct minheap *heap, uint32_t idx) {
    uint32_t left, right, smallest;

    while (true) {
        left = 2 * idx + 1;
        right = 2 * idx + 2;
        smallest = idx;

        uint32_t heap_size = MINHEAP_SIZE(heap);
        if (left < heap_size && MINHEAP_NODE_KEY(heap->nodes[left]) <
                                    MINHEAP_NODE_KEY(heap->nodes[smallest]))
            smallest = left;

        if (right < heap_size && MINHEAP_NODE_KEY(heap->nodes[right]) <
                                     MINHEAP_NODE_KEY(heap->nodes[smallest]))
            smallest = right;

        if (smallest == idx)
            break;

        minheap_swap(heap, idx, smallest);
        idx = smallest;
    }
}

struct minheap *minheap_create(void) {
    struct minheap *heap = vnew(heap);
    if (!heap)
        return NULL;

    MINHEAP_SET_CAPACITY(heap, MINHEAP_INIT_CAP);
    MINHEAP_SET_SIZE(heap, 0);
    heap->nodes = vnews(heap->nodes, MINHEAP_CAPACITY(heap), ALLOC_ZERO);
    if (!heap->nodes) {
        vfree(heap);
        return NULL;
    }

    spinlock_init(&heap->lock);
    return heap;
}

void minheap_expand(struct minheap *heap, uint32_t new_size) {
    if (new_size <= MINHEAP_CAPACITY(heap))
        return;

    struct minheap_node **new_nodes = vnews(new_nodes, new_size);

    if (!new_nodes)
        return;

    memcpy(new_nodes, heap->nodes,
           sizeof(struct minheap_node *) * MINHEAP_CAPACITY(heap));

    vfree(heap->nodes);
    heap->nodes = new_nodes;
    MINHEAP_SET_CAPACITY(heap, new_size);
}

void minheap_insert(struct minheap *heap, struct minheap_node *node,
                    uint64_t key) {
    enum irql irql = spin_lock(&heap->lock);
    uint32_t heap_cap = MINHEAP_CAPACITY(heap);
    uint32_t heap_size = MINHEAP_SIZE(heap);
    if (heap_size >= heap_cap) {
        uint32_t new_cap = heap_cap * 2;
        struct minheap_node **new_nodes = vnews(new_nodes, new_cap);

        if (!new_nodes) {
            spin_unlock(&heap->lock, irql);
            return;
        }

        memcpy(new_nodes, heap->nodes,
               sizeof(struct minheap_node *) * heap_cap);
        vfree(heap->nodes);
        heap->nodes = new_nodes;
        MINHEAP_SET_CAPACITY(heap, new_cap);
    }

    MINHEAP_NODE_SET_INDEX(node, heap_size);
    MINHEAP_NODE_SET_KEY(node, key);

    heap->nodes[heap_size] = node;
    MINHEAP_SET_SIZE(heap, heap_size + 1);

    minheap_sift_up(heap, MINHEAP_NODE_INDEX(node));
    spin_unlock(&heap->lock, irql);
}

void minheap_remove(struct minheap *heap, struct minheap_node *node) {
    enum irql irql = spin_lock(&heap->lock);
    uint32_t idx = MINHEAP_NODE_INDEX(node);
    uint32_t heap_size = MINHEAP_SIZE(heap);

    if (idx >= heap_size) {
        spin_unlock(&heap->lock, irql);
        return;
    }

    heap_size--;
    MINHEAP_SET_SIZE(heap, heap_size);
    if (idx != heap_size) {
        heap->nodes[idx] = heap->nodes[heap_size];
        MINHEAP_NODE_SET_INDEX(heap->nodes[idx], idx);
        minheap_sift_down(heap, idx);
        minheap_sift_up(heap, idx);
    }

    MINHEAP_MARK_NODE_INVALID(node);
    spin_unlock(&heap->lock, irql);
}

struct minheap_node *minheap_pop(struct minheap *heap) {
    uint32_t heap_size = MINHEAP_SIZE(heap);
    if (heap_size == 0)
        return NULL;

    struct minheap_node *top = heap->nodes[0];
    heap_size--;
    MINHEAP_SET_SIZE(heap, heap_size);
    if (heap_size > 0) {
        heap->nodes[0] = heap->nodes[heap_size];
        MINHEAP_NODE_SET_INDEX(heap->nodes[0], 0);
        minheap_sift_down(heap, 0);
    }

    MINHEAP_MARK_NODE_INVALID(top);
    return top;
}
