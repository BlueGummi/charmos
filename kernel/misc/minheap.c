#include <console/panic.h>
#include <mem/alloc.h>
#include <misc/minheap.h>
#include <string.h>

static void minheap_swap(struct minheap *heap, uint32_t a, uint32_t b) {
    struct minheap_node *tmp = heap->nodes[a];
    heap->nodes[a] = heap->nodes[b];
    heap->nodes[b] = tmp;

    heap->nodes[a]->index = a;
    heap->nodes[b]->index = b;
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
    struct minheap *heap = kmalloc(sizeof(struct minheap));
    heap->capacity = MINHEAP_INIT_CAP;
    heap->size = 0;
    heap->nodes = kzalloc(sizeof(struct minheap_node *) * heap->capacity);
    return heap;
}

void minheap_expand(struct minheap *heap, uint32_t new_size) {
    if (new_size <= heap->capacity)
        return;

    struct minheap_node **new_nodes =
        kmalloc(sizeof(struct minheap_node *) * new_size);

    if (!new_nodes)
        return;

    memcpy(new_nodes, heap->nodes,
           sizeof(struct minheap_node *) * heap->capacity);

    kfree(heap->nodes);
    heap->nodes = new_nodes;
    heap->capacity = new_size;
}

void minheap_insert(struct minheap *heap, struct minheap_node *node,
                    uint64_t key) {
    if (heap->size >= heap->capacity) {
        uint32_t new_cap = heap->capacity * 2;
        struct minheap_node **new_nodes =
            kmalloc(sizeof(struct minheap_node *) * new_cap);

        if (!new_nodes)
            return;

        memcpy(new_nodes, heap->nodes,
               sizeof(struct minheap_node *) * heap->capacity);
        kfree(heap->nodes);
        heap->nodes = new_nodes;
        heap->capacity = new_cap;
    }

    node->key = key;
    node->index = heap->size;
    heap->nodes[heap->size++] = node;

    minheap_sift_up(heap, node->index);
}

void minheap_remove(struct minheap *heap, struct minheap_node *node) {
    uint32_t idx = node->index;
    if (idx >= heap->size) {
        k_panic("Minheap node index '%u' exceeds minheap capacity!", idx);
        return;
    }

    heap->size--;
    if (idx != heap->size) {
        heap->nodes[idx] = heap->nodes[heap->size];
        heap->nodes[idx]->index = idx;
        minheap_sift_down(heap, idx);
        minheap_sift_up(heap, idx);
    }

    node->index = MINHEAP_INDEX_INVALID;
}

struct minheap_node *minheap_pop(struct minheap *heap) {
    if (heap->size == 0)
        return NULL;

    struct minheap_node *top = heap->nodes[0];
    heap->size--;
    if (heap->size > 0) {
        heap->nodes[0] = heap->nodes[heap->size];
        heap->nodes[0]->index = 0;
        minheap_sift_down(heap, 0);
    }

    top->index = MINHEAP_INDEX_INVALID;
    return top;
}
