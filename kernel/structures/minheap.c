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
        if (heap->nodes[idx]->key >= heap->nodes[parent]->key)
            break;

        minheap_swap(heap, idx, parent);
        idx = parent;
    }
}

static void minheap_sift_down(struct minheap *heap, uint32_t idx) {
    uint32_t left, right, smallest;

    while (1) {
        left = 2 * idx + 1;
        right = 2 * idx + 2;
        smallest = idx;

        if (left < heap->size &&
            heap->nodes[left]->key < heap->nodes[smallest]->key)
            smallest = left;

        if (right < heap->size &&
            heap->nodes[right]->key < heap->nodes[smallest]->key)
            smallest = right;

        if (smallest == idx)
            break;

        minheap_swap(heap, idx, smallest);
        idx = smallest;
    }
}

struct minheap *minheap_create(void) {
    struct minheap *heap =
        kmalloc(sizeof(struct minheap), ALLOC_PARAMS_DEFAULT);
    heap->capacity = MINHEAP_INIT_CAP;
    heap->size = 0;
    heap->nodes = kzalloc(sizeof(struct minheap_node *) * heap->capacity,
                          ALLOC_PARAMS_DEFAULT);
    return heap;
}

void minheap_expand(struct minheap *heap, uint32_t new_size) {
    if (new_size <= heap->capacity)
        return;

    struct minheap_node **new_nodes =
        kmalloc(sizeof(struct minheap_node *) * new_size, ALLOC_PARAMS_DEFAULT);

    if (!new_nodes)
        return;

    memcpy(new_nodes, heap->nodes,
           sizeof(struct minheap_node *) * heap->capacity);

    kfree(heap->nodes, FREE_PARAMS_DEFAULT);
    heap->nodes = new_nodes;
    MINHEAP_SET_CAPACITY(heap, new_size);
}

void minheap_insert(struct minheap *heap, struct minheap_node *node,
                    uint64_t key) {
    enum irql irql = minheap_node_lock(node);
    if (heap->size >= heap->capacity) {
        uint32_t new_cap = heap->capacity * 2;
        struct minheap_node **new_nodes = kmalloc(
            sizeof(struct minheap_node *) * new_cap, ALLOC_PARAMS_DEFAULT);

        if (!new_nodes)
            return;

        memcpy(new_nodes, heap->nodes,
               sizeof(struct minheap_node *) * heap->capacity);
        kfree(heap->nodes, FREE_PARAMS_DEFAULT);
        heap->nodes = new_nodes;
        heap->capacity = new_cap;
    }

    MINHEAP_NODE_SET_INDEX(node, heap->size);
    MINHEAP_NODE_SET_KEY(node, key);

    heap->nodes[heap->size++] = node;

    minheap_sift_up(heap, node->index);
    minheap_node_unlock(node, irql);
}

void minheap_remove(struct minheap *heap, struct minheap_node *node) {
    enum irql irql = minheap_node_lock(node);
    uint32_t idx = MINHEAP_NODE_INDEX(node);

    if (idx >= MINHEAP_SIZE(heap)) {
        minheap_node_unlock(node, irql);
        return;
    }

    heap->size--;
    if (idx != heap->size) {
        heap->nodes[idx] = heap->nodes[heap->size];
        heap->nodes[idx]->index = idx;
        minheap_sift_down(heap, idx);
        minheap_sift_up(heap, idx);
    }

    MINHEAP_MARK_NODE_INVALID(node);
    minheap_node_unlock(node, irql);
}

struct minheap_node *minheap_pop(struct minheap *heap) {
    if (MINHEAP_SIZE(heap) == 0)
        return NULL;

    struct minheap_node *top = heap->nodes[0];
    heap->size--;
    if (heap->size > 0) {
        heap->nodes[0] = heap->nodes[heap->size];
        heap->nodes[0]->index = 0;
        minheap_sift_down(heap, 0);
    }

    MINHEAP_MARK_NODE_INVALID(top);
    return top;
}
