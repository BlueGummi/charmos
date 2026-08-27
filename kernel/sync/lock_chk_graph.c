#ifdef DEBUG_LOCK_CHK

#include <irq/irq.h>
#include <kassert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lock_chk_internal.h"

static size_t lock_chk_class_hash(const struct lock_chk_class *class,
                                  uint8_t subclass) {
    uintptr_t key = (uintptr_t) class;
    return ((key >> 4) ^ (key >> 13) ^ subclass) & (LOCK_CHK_HASH_BUCKETS - 1);
}

static struct lock_chk_node *lock_chk_hash_next(struct lock_chk_node *node) {
    if (node->hash_entry.next == NULL)
        return NULL;

    return hlist_entry(node->hash_entry.next, struct lock_chk_node, hash_entry);
}

static struct lock_chk_node *
lock_chk_graph_find_node_locked(struct lock_chk_graph *graph,
                                const struct lock_chk_class *class,
                                uint8_t subclass) {
    size_t bucket = lock_chk_class_hash(class, subclass);
    struct hlist_node *first = graph->class_hash[bucket].first;
    struct lock_chk_node *node =
        first != NULL ? hlist_entry(first, struct lock_chk_node, hash_entry)
                      : NULL;

    for (; node != NULL; node = lock_chk_hash_next(node))
        if (node->class == class && node->subclass == subclass)
            return node;

    return NULL;
}

static enum lock_chk_result lock_chk_graph_record_context_locked(
    struct lock_chk_node *base_node,
    const struct lock_chk_acquire_request *request) {
    if (request->type != LOCK_CHK_TYPE_SPIN &&
        request->type != LOCK_CHK_TYPE_QSPIN)
        return LOCK_CHK_RESULT_OK;

    uint8_t ctx = 0;
    if (request->in_irq || !irq_in_thread_context()) {
        ctx = LOCK_CHK_CTX_IRQ;
    } else if (request->raw_operation) {
        if (request->prev_irql >= IRQL_HIGH_LEVEL || !request->irqs_enabled)
            ctx = LOCK_CHK_CTX_SPIN_HIGH;
        else
            ctx = LOCK_CHK_CTX_SPIN_DISPATCH;
    } else if (request->irq_safe) {
        ctx = LOCK_CHK_CTX_SPIN_HIGH;
    } else {
        ctx = LOCK_CHK_CTX_SPIN_DISPATCH;
    }

    /* IRQ safety mismatch check */
    if ((ctx == LOCK_CHK_CTX_SPIN_DISPATCH &&
         (base_node->context_bits &
          (LOCK_CHK_CTX_SPIN_HIGH | LOCK_CHK_CTX_IRQ)) != 0) ||
        ((ctx == LOCK_CHK_CTX_SPIN_HIGH || ctx == LOCK_CHK_CTX_IRQ) &&
         (base_node->context_bits & LOCK_CHK_CTX_SPIN_DISPATCH) != 0)) {
        return LOCK_CHK_RESULT_BAD_CONTEXT;
    }

    base_node->context_bits |= ctx;
    return LOCK_CHK_RESULT_OK;
}

static enum lock_chk_result lock_chk_graph_resolve_node_locked(
    struct lock_chk_graph *graph, struct lock_chk_map *map, uint8_t subclass,
    const struct lock_chk_acquire_request *request,
    struct lock_chk_node **out) {
    if (subclass >= LOCK_CHK_MAX_SUBCLASSES)
        return LOCK_CHK_RESULT_INTERNAL;

    const struct lock_chk_class *class =
        map->class != NULL ? map->class : &map->instance_class;

    struct lock_chk_node *base_node =
        lock_chk_graph_find_node_locked(graph, class, 0);
    if (base_node == NULL) {
        if (graph->node_count == LOCK_CHK_MAX_NODES)
            return LOCK_CHK_RESULT_NODE_CAPACITY;

        base_node = &graph->nodes[graph->node_count];
        base_node->id = graph->node_count++;
        base_node->class = class;
        base_node->subclass = 0;
        base_node->context_bits = 0;
        INIT_HLIST_NODE(&base_node->hash_entry);
        INIT_LIST_HEAD(&base_node->out_edges);
        INIT_LIST_HEAD(&base_node->in_edges);
        hlist_add_head(&base_node->hash_entry,
                       &graph->class_hash[lock_chk_class_hash(class, 0)]);
    }

    atomic_store_explicit(&map->base_node, base_node, memory_order_release);

    if (request != NULL) {
        enum lock_chk_result ctx_res =
            lock_chk_graph_record_context_locked(base_node, request);
        if (ctx_res != LOCK_CHK_RESULT_OK)
            return ctx_res;
    }

    if (subclass == 0) {
        *out = base_node;
        return LOCK_CHK_RESULT_OK;
    }

    struct lock_chk_node *node =
        lock_chk_graph_find_node_locked(graph, class, subclass);
    if (node == NULL) {
        if (graph->node_count == LOCK_CHK_MAX_NODES)
            return LOCK_CHK_RESULT_NODE_CAPACITY;

        node = &graph->nodes[graph->node_count];
        node->id = graph->node_count++;
        node->class = class;
        node->subclass = subclass;
        node->context_bits = 0;
        INIT_HLIST_NODE(&node->hash_entry);
        INIT_LIST_HEAD(&node->out_edges);
        INIT_LIST_HEAD(&node->in_edges);
        hlist_add_head(
            &node->hash_entry,
            &graph->class_hash[lock_chk_class_hash(class, subclass)]);
    }

    *out = node;
    return LOCK_CHK_RESULT_OK;
}

static struct lock_chk_edge *lock_chk_graph_find_edge_locked(
    struct lock_chk_node *from, enum lock_chk_mode from_mode,
    struct lock_chk_node *to, enum lock_chk_mode to_mode) {
    struct list_head *entry;
    list_for_each(entry, &from->out_edges) {
        struct lock_chk_edge *edge =
            list_entry(entry, struct lock_chk_edge, from_entry);
        if (edge->to == to && edge->from_mode == from_mode &&
            edge->to_mode == to_mode)
            return edge;
    }

    return NULL;
}

static uint16_t lock_chk_mode_state(const struct lock_chk_node *node,
                                    enum lock_chk_mode mode) {
    return (uint16_t) (node->id * 2 + mode);
}

static struct lock_chk_node *lock_chk_state_node(struct lock_chk_graph *graph,
                                                 uint16_t state) {
    return &graph->nodes[state / 2];
}

static enum lock_chk_mode lock_chk_state_mode(uint16_t state) {
    return (enum lock_chk_mode)(state % 2);
}

static bool lock_chk_graph_path_locked(struct lock_chk_graph *graph,
                                       struct lock_chk_node *start,
                                       enum lock_chk_mode start_mode,
                                       struct lock_chk_node *target,
                                       enum lock_chk_mode target_mode) {
    graph->generation++;
    if (graph->generation == 0) {
        memset(graph->visit_generation, 0, sizeof(graph->visit_generation));
        graph->generation = 1;
    }

    uint16_t stack_depth = 0;
    uint16_t start_state = lock_chk_mode_state(start, start_mode);
    graph->dfs_stack[stack_depth++] = start_state;
    graph->visit_generation[start_state] = graph->generation;
    graph->parent_state[start_state] = -1;
    graph->parent_edge[start_state] = -1;

    while (stack_depth != 0) {
        uint16_t state = graph->dfs_stack[--stack_depth];
        struct lock_chk_node *node = lock_chk_state_node(graph, state);
        enum lock_chk_mode mode = lock_chk_state_mode(state);

        if (node == target && lock_chk_modes_conflict(mode, target_mode)) {
            graph->last_cycle_end_state = (int32_t) state;
            return true;
        }

        struct list_head *entry;
        list_for_each(entry, &node->out_edges) {
            struct lock_chk_edge *edge =
                list_entry(entry, struct lock_chk_edge, from_entry);
            if (!lock_chk_modes_conflict(mode, edge->from_mode))
                continue;

            uint16_t next = lock_chk_mode_state(edge->to, edge->to_mode);
            if (graph->visit_generation[next] == graph->generation)
                continue;

            graph->visit_generation[next] = graph->generation;
            graph->parent_state[next] = state;
            graph->parent_edge[next] = (int32_t) (edge - &graph->edges[0]);
            graph->dfs_stack[stack_depth++] = next;
        }
    }

    return false;
}

static void lock_chk_graph_publish_edge_locked(
    struct lock_chk_graph *graph, struct lock_chk_node *from,
    enum lock_chk_mode from_mode, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site) {
    struct lock_chk_edge *edge = &graph->edges[graph->edge_count++];
    edge->from = from;
    edge->to = to;
    edge->from_mode = from_mode;
    edge->to_mode = to_mode;
    edge->first_site = site;
    INIT_LIST_HEAD(&edge->from_entry);
    INIT_LIST_HEAD(&edge->to_entry);
    list_add_tail(&edge->from_entry, &from->out_edges);
    list_add_tail(&edge->to_entry, &to->in_edges);
}

uint16_t lock_chk_graph_extract_cycle_locked(
    struct lock_chk_graph *graph, struct lock_chk_node *from,
    enum lock_chk_mode from_mode, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_cycle_hop *hops, uint16_t max_hops, bool *truncated) {
    if (max_hops == 0)
        return 0;

    uint16_t edge_indices[LOCK_CHK_MAX_CYCLE_HOPS];
    uint16_t num_existing_edges = 0;
    int32_t curr = graph->last_cycle_end_state;

    while (curr >= 0 && graph->parent_state[curr] != -1) {
        int32_t e_idx = graph->parent_edge[curr];
        if (e_idx >= 0 && num_existing_edges < LOCK_CHK_MAX_CYCLE_HOPS) {
            edge_indices[num_existing_edges++] = (uint16_t) e_idx;
        }
        curr = graph->parent_state[curr];
    }

    hops[0] = (struct lock_chk_cycle_hop){
        .from_class = from->class,
        .from_subclass = from->subclass,
        .from_mode = from_mode,
        .to_class = to->class,
        .to_subclass = to->subclass,
        .to_mode = to_mode,
        .site = site,
    };
    uint16_t count = 1;

    for (int i = (int) num_existing_edges - 1; i >= 0 && count < max_hops;
         i--) {
        struct lock_chk_edge *e = &graph->edges[edge_indices[i]];
        hops[count++] = (struct lock_chk_cycle_hop){
            .from_class = e->from->class,
            .from_subclass = e->from->subclass,
            .from_mode = e->from_mode,
            .to_class = e->to->class,
            .to_subclass = e->to->subclass,
            .to_mode = e->to_mode,
            .site = e->first_site,
        };
    }

    if (truncated != NULL)
        *truncated = (num_existing_edges + 1 > max_hops);

    return count;
}

static int lock_chk_compare_hops(const struct lock_chk_cycle_hop *a,
                                 const struct lock_chk_cycle_hop *b) {
    const char *from_a_name = a->from_class ? a->from_class->name : "";
    const char *from_b_name = b->from_class ? b->from_class->name : "";
    int c = strcmp(from_a_name, from_b_name);
    if (c != 0)
        return c;

    const char *from_a_file = a->from_class ? a->from_class->file : "";
    const char *from_b_file = b->from_class ? b->from_class->file : "";
    c = strcmp(from_a_file, from_b_file);
    if (c != 0)
        return c;

    uint32_t from_a_line = a->from_class ? a->from_class->line : 0;
    uint32_t from_b_line = b->from_class ? b->from_class->line : 0;
    if (from_a_line != from_b_line)
        return from_a_line < from_b_line ? -1 : 1;

    if (a->from_subclass != b->from_subclass)
        return a->from_subclass < b->from_subclass ? -1 : 1;

    if (a->from_mode != b->from_mode)
        return a->from_mode < b->from_mode ? -1 : 1;

    const char *to_a_name = a->to_class ? a->to_class->name : "";
    const char *to_b_name = b->to_class ? b->to_class->name : "";
    c = strcmp(to_a_name, to_b_name);
    if (c != 0)
        return c;

    const char *to_a_file = a->to_class ? a->to_class->file : "";
    const char *to_b_file = b->to_class ? b->to_class->file : "";
    c = strcmp(to_a_file, to_b_file);
    if (c != 0)
        return c;

    uint32_t to_a_line = a->to_class ? a->to_class->line : 0;
    uint32_t to_b_line = b->to_class ? b->to_class->line : 0;
    if (to_a_line != to_b_line)
        return to_a_line < to_b_line ? -1 : 1;

    if (a->to_subclass != b->to_subclass)
        return a->to_subclass < b->to_subclass ? -1 : 1;

    if (a->to_mode != b->to_mode)
        return a->to_mode < b->to_mode ? -1 : 1;

    return 0;
}

static int lock_chk_compare_rotations(const struct lock_chk_cycle_hop *hops,
                                      uint16_t len, uint16_t rot_a,
                                      uint16_t rot_b) {
    for (uint16_t offset = 0; offset < len; offset++) {
        const struct lock_chk_cycle_hop *hop_a = &hops[(rot_a + offset) % len];
        const struct lock_chk_cycle_hop *hop_b = &hops[(rot_b + offset) % len];
        int c = lock_chk_compare_hops(hop_a, hop_b);
        if (c != 0)
            return c;
    }
    return 0;
}

uint64_t
lock_chk_calc_canonical_cycle_sig(const struct lock_chk_cycle_hop *hops,
                                  uint16_t cycle_len) {
    if (cycle_len == 0)
        return 0;

    uint16_t best_rot = 0;
    for (uint16_t i = 1; i < cycle_len; i++) {
        if (lock_chk_compare_rotations(hops, cycle_len, i, best_rot) < 0)
            best_rot = i;
    }

    uint64_t signature = UINT64_C(14695981039346656037);
    for (uint16_t step = 0; step < cycle_len; step++) {
        const struct lock_chk_cycle_hop *hop =
            &hops[(best_rot + step) % cycle_len];
        const char *from_name = hop->from_class ? hop->from_class->name : "";
        const char *from_file = hop->from_class ? hop->from_class->file : "";
        uint32_t from_line = hop->from_class ? hop->from_class->line : 0;
        const char *to_name = hop->to_class ? hop->to_class->name : "";
        const char *to_file = hop->to_class ? hop->to_class->file : "";
        uint32_t to_line = hop->to_class ? hop->to_class->line : 0;

        signature = lock_chk_hash_string(signature, from_name);
        signature = lock_chk_hash_string(signature, from_file);
        signature =
            lock_chk_hash_bytes(signature, &from_line, sizeof(from_line));
        signature = lock_chk_hash_bytes(signature, &hop->from_subclass,
                                        sizeof(hop->from_subclass));
        signature = lock_chk_hash_bytes(signature, &hop->from_mode,
                                        sizeof(hop->from_mode));
        signature = lock_chk_hash_string(signature, to_name);
        signature = lock_chk_hash_string(signature, to_file);
        signature = lock_chk_hash_bytes(signature, &to_line, sizeof(to_line));
        signature = lock_chk_hash_bytes(signature, &hop->to_subclass,
                                        sizeof(hop->to_subclass));
        signature =
            lock_chk_hash_bytes(signature, &hop->to_mode, sizeof(hop->to_mode));
    }

    return signature;
}

void lock_chk_graph_init(struct lock_chk_graph *graph) {
    raw_spinlock_init(&graph->lock);
    graph->node_count = 0;
    graph->edge_count = 0;
    graph->generation = 0;
    graph->last_cycle_end_state = -1;
    memset(graph->class_hash, 0, sizeof(graph->class_hash));
    memset(graph->visit_generation, 0, sizeof(graph->visit_generation));
}

enum lock_chk_result
lock_chk_graph_resolve_node(struct lock_chk_graph *graph,
                            struct lock_chk_map *map, uint8_t subclass,
                            const struct lock_chk_acquire_request *request,
                            struct lock_chk_node **out) {
    bool irqs_enabled = raw_spin_lock_irq_disable(&graph->lock);
    enum lock_chk_result result =
        lock_chk_graph_resolve_node_locked(graph, map, subclass, request, out);
    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return result;
}

static void lock_chk_graph_fill_cycle_failure(
    struct lock_chk_failure *failure_out, struct lock_chk_graph *graph,
    struct lock_chk_node *from, enum lock_chk_mode from_mode,
    struct lock_chk_node *to, enum lock_chk_mode to_mode,
    const struct lock_chk_site *site) {
    *failure_out = (struct lock_chk_failure){
        .kind = LOCK_CHK_FAIL_CYCLE,
        .site = site,
        .class = to->class,
        .subclass = to->subclass,
        .mode = to_mode,
    };
    snprintf(failure_out->msg, sizeof(failure_out->msg),
             "Dependency cycle detected (%s -> %s)",
             from->class ? from->class->name : "lock",
             to->class ? to->class->name : "lock");
    failure_out->cycle_len = lock_chk_graph_extract_cycle_locked(
        graph, from, from_mode, to, to_mode, site, failure_out->cycle_hops,
        LOCK_CHK_MAX_CYCLE_HOPS, &failure_out->cycle_truncated);
    failure_out->signature = lock_chk_calc_canonical_cycle_sig(
        failure_out->cycle_hops, failure_out->cycle_len);
}

static void lock_chk_graph_fill_edge_capacity_failure(
    struct lock_chk_failure *failure_out, struct lock_chk_graph *graph,
    struct lock_chk_node *to, const struct lock_chk_site *site) {
    *failure_out = (struct lock_chk_failure){
        .kind = LOCK_CHK_FAIL_CAPACITY,
        .site = site,
        .class = to->class,
        .subclass = to->subclass,
        .capacity_pool = "edges",
        .capacity_used = graph->edge_count,
        .capacity_limit = LOCK_CHK_MAX_EDGES,
    };
    snprintf(failure_out->msg, sizeof(failure_out->msg),
             "Edge capacity exhausted (%u/%u)", graph->edge_count,
             LOCK_CHK_MAX_EDGES);
}

enum lock_chk_result lock_chk_graph_add_dependency(
    struct lock_chk_graph *graph, struct lock_chk_node *from,
    enum lock_chk_mode from_mode, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_failure *failure_out) {
    bool irqs_enabled = raw_spin_lock_irq_disable(&graph->lock);
    enum lock_chk_result result = LOCK_CHK_RESULT_OK;

    if (lock_chk_graph_find_edge_locked(from, from_mode, to, to_mode) != NULL)
        goto out;

    if (lock_chk_graph_path_locked(graph, to, to_mode, from, from_mode)) {
        if (failure_out != NULL)
            lock_chk_graph_fill_cycle_failure(failure_out, graph, from,
                                              from_mode, to, to_mode, site);
        result = LOCK_CHK_RESULT_CYCLE;
        goto out;
    }

    if (graph->edge_count == LOCK_CHK_MAX_EDGES) {
        if (failure_out != NULL)
            lock_chk_graph_fill_edge_capacity_failure(failure_out, graph, to,
                                                      site);
        result = LOCK_CHK_RESULT_EDGE_CAPACITY;
        goto out;
    }

    lock_chk_graph_publish_edge_locked(graph, from, from_mode, to, to_mode,
                                       site);

out:
    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return result;
}

static bool lock_chk_graph_dependency_repeated(
    const struct lock_chk_thread_data *thread_data, uint8_t index) {
    const struct lock_chk_held *candidate = &thread_data->held[index];

    for (uint8_t i = 0; i < index; i++) {
        const struct lock_chk_held *prior = &thread_data->held[i];
        if ((prior->flags & LOCK_CHKD_ORDER) != 0 &&
            prior->node == candidate->node && prior->mode == candidate->mode)
            return true;
    }

    return false;
}

static enum lock_chk_result lock_chk_graph_add_held_dependencies_locked(
    struct lock_chk_graph *graph,
    const struct lock_chk_thread_data *thread_data, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_failure *failure_out) {
    size_t missing = 0;

    for (uint8_t i = 0; i < thread_data->depth; i++) {
        const struct lock_chk_held *held = &thread_data->held[i];
        if ((held->flags & LOCK_CHKD_ORDER) == 0 ||
            lock_chk_graph_dependency_repeated(thread_data, i))
            continue;

        if (lock_chk_graph_find_edge_locked(held->node, held->mode, to,
                                            to_mode) != NULL)
            continue;

        if (lock_chk_graph_path_locked(graph, to, to_mode, held->node,
                                       held->mode)) {
            if (failure_out != NULL)
                lock_chk_graph_fill_cycle_failure(failure_out, graph,
                                                  held->node, held->mode, to,
                                                  to_mode, site);
            return LOCK_CHK_RESULT_CYCLE;
        }
        missing++;
    }

    if (missing > (size_t) (LOCK_CHK_MAX_EDGES - graph->edge_count)) {
        if (failure_out != NULL)
            lock_chk_graph_fill_edge_capacity_failure(failure_out, graph, to,
                                                      site);
        return LOCK_CHK_RESULT_EDGE_CAPACITY;
    }

    for (uint8_t i = 0; i < thread_data->depth; i++) {
        const struct lock_chk_held *held = &thread_data->held[i];
        if ((held->flags & LOCK_CHKD_ORDER) == 0 ||
            lock_chk_graph_dependency_repeated(thread_data, i) ||
            lock_chk_graph_find_edge_locked(held->node, held->mode, to,
                                            to_mode) != NULL)
            continue;

        lock_chk_graph_publish_edge_locked(graph, held->node, held->mode, to,
                                           to_mode, site);
    }

    return LOCK_CHK_RESULT_OK;
}

enum lock_chk_result lock_chk_graph_add_held_dependencies(
    struct lock_chk_graph *graph,
    const struct lock_chk_thread_data *thread_data, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_failure *failure_out) {
    bool irqs_enabled = raw_spin_lock_irq_disable(&graph->lock);
    enum lock_chk_result result = lock_chk_graph_add_held_dependencies_locked(
        graph, thread_data, to, to_mode, site, failure_out);
    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return result;
}

static void lock_chk_graph_rollback_nodes(struct lock_chk_graph *graph,
                                          uint16_t old_node_count) {
    while (graph->node_count > old_node_count) {
        struct lock_chk_node *node = &graph->nodes[--graph->node_count];
        hlist_del(&node->hash_entry);
    }
}

enum lock_chk_result lock_chk_graph_prepare_acquire(
    struct lock_chk_graph *graph, struct lock_chk_map *map, uint8_t subclass,
    const struct lock_chk_acquire_request *request,
    const struct lock_chk_thread_data *thread_data, struct lock_chk_node **out,
    struct lock_chk_failure *failure_out) {
    bool irqs_enabled = raw_spin_lock_irq_disable(&graph->lock);
    uint16_t old_node_count = graph->node_count;
    struct lock_chk_node *old_base =
        atomic_load_explicit(&map->base_node, memory_order_relaxed);
    const struct lock_chk_class *class =
        map->class != NULL ? map->class : &map->instance_class;
    struct lock_chk_node *existing_base =
        lock_chk_graph_find_node_locked(graph, class, 0);
    uint8_t old_context_bits =
        existing_base != NULL ? existing_base->context_bits : 0;

    enum lock_chk_result result =
        lock_chk_graph_resolve_node_locked(graph, map, subclass, request, out);
    if (result != LOCK_CHK_RESULT_OK)
        goto rollback;

    if (request->wait_kind == LOCK_CHK_WAIT_BLOCKING) {
        result = lock_chk_graph_add_held_dependencies_locked(
            graph, thread_data, *out, request->mode, request->site,
            failure_out);
        if (result != LOCK_CHK_RESULT_OK)
            goto rollback;
    }

    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return LOCK_CHK_RESULT_OK;

rollback:
    lock_chk_graph_rollback_nodes(graph, old_node_count);
    if (existing_base != NULL)
        existing_base->context_bits = old_context_bits;
    atomic_store_explicit(&map->base_node, old_base, memory_order_relaxed);
    *out = NULL;
    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return result;
}

#endif /* DEBUG_LOCK_CHK */
