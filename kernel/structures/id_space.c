#include <kassert.h>
#include <mem/alloc.h>
#include <string.h>
#include <structures/id_space.h>

static size_t id_space_get_data(struct rbt_node *node) {
    return container_of(node, struct id_range, node)->start;
}

static int32_t id_space_cmp(const struct rbt_node *a,
                            const struct rbt_node *b) {
    size_t l = id_space_get_data((void *) a);
    size_t r = id_space_get_data((void *) b);
    if (l < r) {
        return -1;
    }
    if (l > r) {
        return 1;
    }
    return 0;
}

struct id_space *id_space_init(uint64_t max_id) {
    struct id_space *is = kmalloc(sizeof(*is), ALLOC_FLAGS_ZERO);
    if (!is) {
        return NULL;
    }

    rbt_init(&is->tree, id_space_get_data, id_space_cmp);
    spinlock_init(&is->lock);

    is->reserve_free = NULL;
    for (int i = 0; i < ID_RANGE_RESERVE_COUNT; i++) {
        rbt_init_node(&is->reserve_pool[i].node);
        is->reserve_pool[i].start = 0;
        is->reserve_pool[i].length = 0;
        is->reserve_pool[i].next = is->reserve_free;
        is->reserve_free = &is->reserve_pool[i];
    }

    struct id_range *r = kmalloc(sizeof(*r), ALLOC_FLAGS_ZERO);
    if (!r) {
        if (is->reserve_free) {
            r = is->reserve_free;
            is->reserve_free = r->next;
        } else {
            return is;
        }
    }

    r->start = 1;
    r->length = max_id;
    rbt_insert(&is->tree, &r->node);

    return is;
}

static struct id_range *id_range_alloc(struct id_space *is) {
    SPINLOCK_ASSERT_HELD(&is->lock);
    struct id_range *r = kmalloc(sizeof(*r), ALLOC_FLAGS_ZERO);
    if (r) {
        return r;
    }

    if (is->reserve_free) {
        r = is->reserve_free;
        is->reserve_free = r->next;
        memset(r, 0, sizeof(*r));
        return r;
    }

    return NULL;
}

static void id_range_free(struct id_space *is, struct id_range *r) {
    SPINLOCK_ASSERT_HELD(&is->lock);
    if ((uintptr_t) r >= (uintptr_t) &is->reserve_pool[0] &&
        (uintptr_t) r < (uintptr_t) &is->reserve_pool[ID_RANGE_RESERVE_COUNT]) {
        r->next = is->reserve_free;
        is->reserve_free = r;
    } else {
        kfree(r);
    }
}

uint64_t id_space_alloc(struct id_space *is) {
    enum irql irql = spin_lock(&is->lock);

    struct rbt_node *node = rbt_min(&is->tree);
    if (!node) {
        spin_unlock(&is->lock, irql);
        return 0;
    }

    struct id_range *range = rbt_entry(node, struct id_range, node);
    uint64_t id = range->start;

    if (range->length == 1) {
        rbt_delete(&is->tree, node);
        id_range_free(is, range);
    } else {
        rbt_delete(&is->tree, &range->node);
        range->start++;
        range->length--;
        rbt_insert(&is->tree, &range->node);
    }

    spin_unlock(&is->lock, irql);
    return id;
}

void id_space_free(struct id_space *is, uint64_t id) {
    enum irql irql = spin_lock(&is->lock);

    struct rbt_node *node = is->tree.root;
    struct id_range *prev = NULL;
    struct id_range *next = NULL;

    while (node) {
        struct id_range *r = rbt_entry(node, struct id_range, node);
        if (id < r->start) {
            next = r;
            node = node->left;
        } else if (id > r->start + r->length - 1) {
            prev = r;
            node = node->right;
        } else {
            goto out;
        }
    }

    bool merged_prev = false, merged_next = false;

    if (prev && prev->start + prev->length == id) {
        prev->length++;
        merged_prev = true;
    }

    if (next && next->start == id + 1) {
        if (merged_prev) {
            prev->length += next->length;
            rbt_delete(&is->tree, &next->node);
            id_range_free(is, next);
        } else {
            rbt_delete(&is->tree, &next->node);
            next->start = id;
            next->length++;
            rbt_insert(&is->tree, &next->node);
        }
        merged_next = true;
    }

    if (!merged_prev && !merged_next) {
        struct id_range *new_range = id_range_alloc(is);
        if (!new_range) {
            goto out;
        }

        new_range->start = id;
        new_range->length = 1;
        rbt_insert(&is->tree, &new_range->node);
    }

out:
    spin_unlock(&is->lock, irql);
}

uint64_t id_space_alloc_range(struct id_space *is, uint64_t count) {
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        return id_space_alloc(is);
    }

    enum irql irql = spin_lock(&is->lock);

    struct rbt_node *node;
    struct id_range *range = NULL;

    rbt_for_each(node, &is->tree) {
        struct id_range *r = rbt_entry(node, struct id_range, node);
        if (r->length >= count) {
            range = r;
            break;
        }
    }

    if (!range) {
        spin_unlock(&is->lock, irql);
        return 0;
    }

    uint64_t id = range->start;

    if (range->length == count) {
        rbt_delete(&is->tree, &range->node);
        id_range_free(is, range);
    } else {
        rbt_delete(&is->tree, &range->node);
        range->start += count;
        range->length -= count;
        rbt_insert(&is->tree, &range->node);
    }

    spin_unlock(&is->lock, irql);
    return id;
}

void id_space_free_range(struct id_space *is, uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        id_space_free(is, start + i);
    }
}

void id_space_destroy(struct id_space *is) {
    if (!is) {
        return;
    }

    enum irql irql = spin_lock(&is->lock);
    struct rbt_node *node;
    struct rbt_node *tmp;

    rbt_for_each_safe(node, tmp, &is->tree) {
        struct id_range *r = rbt_entry(node, struct id_range, node);
        rbt_delete(&is->tree, node);
        id_range_free(is, r);
    }
    spin_unlock(&is->lock, irql);

    kfree(is);
}
