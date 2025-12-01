#include <kassert.h>
#include <mem/alloc.h>
#include <sch/tid.h>
#include <string.h>

static size_t tid_space_get_data(struct rbt_node *node) {
    return container_of(node, struct tid_range, node)->start;
}

struct tid_space *tid_space_init(uint64_t max_id) {
    struct tid_space *ts = kzalloc(sizeof(*ts), ALLOC_PARAMS_DEFAULT);
    if (!ts)
        return NULL;

    ts->tree.root = NULL;
    ts->tree.get_data = tid_space_get_data;
    spinlock_init(&ts->lock);

    // Initialize reserve pool
    ts->reserve_free = NULL;
    for (int i = 0; i < TID_RANGE_RESERVE_COUNT; i++) {
        ts->reserve_pool[i].node.left = NULL;
        ts->reserve_pool[i].node.right = NULL;
        ts->reserve_pool[i].node.parent = NULL;
        ts->reserve_pool[i].start = 0;
        ts->reserve_pool[i].length = 0;
        ts->reserve_pool[i].next = ts->reserve_free;
        ts->reserve_free = &ts->reserve_pool[i];
    }

    struct tid_range *r = kzalloc(sizeof(*r), ALLOC_PARAMS_DEFAULT);
    if (!r)
        return ts;

    r->start = 1;
    r->length = max_id;
    rbt_insert(&ts->tree, &r->node);

    return ts;
}

static struct tid_range *tid_range_alloc(struct tid_space *ts) {
    kassert(spinlock_held(&ts->lock));
    struct tid_range *r = kzalloc(sizeof(*r), ALLOC_PARAMS_DEFAULT);
    if (r)
        return r;

    if (ts->reserve_free) {
        r = ts->reserve_free;
        ts->reserve_free = r->next;
        memset(r, 0, sizeof(*r));
        return r;
    }

    return NULL;
}

static void tid_range_free(struct tid_space *ts, struct tid_range *r) {
    kassert(spinlock_held(&ts->lock));
    if ((uintptr_t) r >= (uintptr_t) &ts->reserve_pool[0] &&
        (uintptr_t) r <
            (uintptr_t) &ts->reserve_pool[TID_RANGE_RESERVE_COUNT]) {
        r->next = ts->reserve_free;
        ts->reserve_free = r;
    } else {
        kfree(r, FREE_PARAMS_DEFAULT);
    }
}

uint64_t tid_alloc(struct tid_space *ts) {
    enum irql irql = spin_lock(&ts->lock);

    struct rbt_node *node = rbt_min(&ts->tree);
    if (!node) {
        spin_unlock(&ts->lock, irql);
        return 0;
    }

    struct tid_range *range = rbt_entry(node, struct tid_range, node);
    uint64_t id = range->start;

    if (range->length == 1) {
        rb_delete(&ts->tree, node);
        tid_range_free(ts, range);
    } else {
        range->start++;
        range->length--;
    }

    spin_unlock(&ts->lock, irql);
    return id;
}

void tid_free(struct tid_space *ts, uint64_t id) {
    enum irql irql = spin_lock(&ts->lock);

    struct rbt_node *node = ts->tree.root;
    struct tid_range *prev = NULL;
    struct tid_range *next = NULL;

    while (node) {
        struct tid_range *r = rbt_entry(node, struct tid_range, node);
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
            rb_delete(&ts->tree, &next->node);
            kfree(next, FREE_PARAMS_DEFAULT);
        } else {
            next->start = id;
            next->length++;
        }
        merged_next = true;
    }

    if (!merged_prev && !merged_next) {
        struct tid_range *new_range = tid_range_alloc(ts);

        if (!new_range)
            goto out;

        new_range->start = id;
        new_range->length = 1;
        rbt_insert(&ts->tree, &new_range->node);
    }

out:
    spin_unlock(&ts->lock, irql);
}
