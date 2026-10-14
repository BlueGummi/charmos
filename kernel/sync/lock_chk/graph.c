#ifdef DEBUG_LOCK_CHK

#include <enforce.h>
#include <irq/irq.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

static_assert((LOCK_CHK_HASH_BUCKETS & (LOCK_CHK_HASH_BUCKETS - 1)) == 0,
              "LOCK_CHK_HASH_BUCKETS must be a power of two");

static struct lock_chk_node *node_hash_next(struct lock_chk_node *node) {
    if (node->hash_entry.next == NULL)
        return NULL;

    return hlist_entry(node->hash_entry.next, struct lock_chk_node, hash_entry);
}

static struct lock_chk_node *find_node(struct lock_chk_graph *graph,
                                       const struct lock_chk_class *class,
                                       uint8_t subclass) {
    size_t bucket = lock_chk_class_hash(class, subclass);
    struct hlist_node *first = graph->class_hash[bucket].first;
    struct lock_chk_node *node =
        first != NULL ? hlist_entry(first, struct lock_chk_node, hash_entry)
                      : NULL;

    for (; node != NULL; node = node_hash_next(node))
        if (node->class == class && node->subclass == subclass)
            return node;

    return NULL;
}

static enum lock_chk_result record_ctx(struct lock_chk_node *base_node,
                                       const struct lock_chk_acq_req *request) {
    if (!lock_chk_type_is_spin(request->lock.type))
        return LOCK_CHK_RESULT_OK;

    uint8_t ctx = 0;
    if (request->in_irq || irq_in_interrupt()) {
        ctx = LOCK_CHK_CTX_IRQ;
    } else if (request->op_flags & LOCK_OP_RAW) {
        if (request->prev_irql >= IRQL_HIGH_LEVEL || !request->irqs_enabled) {
            ctx = LOCK_CHK_CTX_SPIN_HIGH;
        } else {
            ctx = LOCK_CHK_CTX_SPIN_DISPATCH;
        }
    } else if ((request->op_flags & LOCK_OP_IRQ_MASK) == LOCK_OP_IRQ_HIGH) {
        ctx = LOCK_CHK_CTX_SPIN_HIGH;
    } else {
        enforce(request->op_flags & LOCK_OP_IRQ_MASK);
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

static enum lock_chk_result resolve_node(struct lock_chk_graph *graph,
                                         struct lock_chk_map *map,
                                         uint8_t subclass,
                                         const struct lock_chk_acq_req *request,
                                         struct lock_chk_node **out) {
    if (subclass >= LOCK_CHK_MAX_SUBCLASSES)
        return LOCK_CHK_RESULT_INTERNAL;

    const struct lock_chk_class *class = lock_chk_map_class(map);

    struct lock_chk_node *base_node = find_node(graph, class, 0);
    if (base_node == NULL) {
        if (graph->node_count == LOCK_CHK_MAX_NODES)
            return LOCK_CHK_RESULT_NODE_CAPACITY;

        base_node = &graph->nodes[graph->node_count];
        base_node->id = graph->node_count++;
        base_node->class = class;
        base_node->subclass = 0;
        base_node->context_bits = 0;
        INIT_HLIST_NODE(&base_node->hash_entry);
        list_head_init(&base_node->out_edges);
        list_head_init(&base_node->in_edges);
        hlist_add_head(&base_node->hash_entry,
                       &graph->class_hash[lock_chk_class_hash(class, 0)]);
    }

    atomic_store_release(&map->base_node, base_node);

    if (request != NULL) {
        enum lock_chk_result ctx_res = record_ctx(base_node, request);
        if (ctx_res != LOCK_CHK_RESULT_OK)
            return ctx_res;
    }

    if (subclass == 0) {
        *out = base_node;
        return LOCK_CHK_RESULT_OK;
    }

    struct lock_chk_node *node = find_node(graph, class, subclass);
    if (node == NULL) {
        if (graph->node_count == LOCK_CHK_MAX_NODES)
            return LOCK_CHK_RESULT_NODE_CAPACITY;

        node = &graph->nodes[graph->node_count];
        node->id = graph->node_count++;
        node->class = class;
        node->subclass = subclass;
        node->context_bits = 0;
        INIT_HLIST_NODE(&node->hash_entry);
        list_head_init(&node->out_edges);
        list_head_init(&node->in_edges);
        hlist_add_head(
            &node->hash_entry,
            &graph->class_hash[lock_chk_class_hash(class, subclass)]);
    }

    *out = node;
    return LOCK_CHK_RESULT_OK;
}

static struct lock_chk_edge *find_edge(struct lock_chk_dep from,
                                       struct lock_chk_dep to) {
    struct list_node *entry;
    list_for_each(entry, &from.node->out_edges) {
        struct lock_chk_edge *edge =
            list_entry(entry, struct lock_chk_edge, from_entry);
        if (edge->to == to.node && edge->from_mode == from.mode &&
            edge->to_mode == to.mode)
            return edge;
    }

    return NULL;
}

static bool graph_path(struct lock_chk_graph *graph, struct lock_chk_dep start,
                       struct lock_chk_dep target) {
    graph->generation++;
    if (graph->generation == 0) {
        memset(graph->visit_generation, 0, sizeof(graph->visit_generation));
        graph->generation = 1;
    }

    uint16_t stack_depth = 0;
    uint16_t start_state = lock_chk_mode_state(start.node, start.mode);
    graph->dfs_stack[stack_depth++] = start_state;
    graph->visit_generation[start_state] = graph->generation;
    graph->parent_state[start_state] = -1;
    graph->parent_edge[start_state] = -1;

    while (stack_depth != 0) {
        uint16_t state = graph->dfs_stack[--stack_depth];
        struct lock_chk_node *node = lock_chk_state_node(graph, state);
        enum lock_chk_mode mode = lock_chk_state_mode(state);

        if (node == target.node && lock_chk_modes_conflict(mode, target.mode)) {
            graph->last_cycle_end_state = (int32_t) state;
            return true;
        }

        struct list_node *entry;
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

static void publish_edge(struct lock_chk_graph *graph, struct lock_chk_dep from,
                         struct lock_chk_dep to,
                         const struct lock_chk_site *site) {
    struct lock_chk_edge *edge = &graph->edges[graph->edge_count++];
    edge->from = from.node;
    edge->to = to.node;
    edge->from_mode = from.mode;
    edge->to_mode = to.mode;
    edge->first_site = site;
    list_node_init(&edge->from_entry);
    list_node_init(&edge->to_entry);
    list_head_push_back(&from.node->out_edges, &edge->from_entry);
    list_head_push_back(&to.node->in_edges, &edge->to_entry);
}

static uint16_t extract_cycle(struct lock_chk_graph *graph,
                              struct lock_chk_dep from, struct lock_chk_dep to,
                              const struct lock_chk_site *site,
                              struct lock_chk_cycle_hop *hops,
                              uint16_t max_hops, bool *truncated) {
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
        .from_class = from.node->class,
        .from_subclass = from.node->subclass,
        .from_mode = from.mode,
        .to_class = to.node->class,
        .to_subclass = to.node->subclass,
        .to_mode = to.mode,
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

static int hops_cmp(const struct lock_chk_cycle_hop *a,
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

static int compare_rotations(const struct lock_chk_cycle_hop *hops,
                             uint16_t len, uint16_t rot_a, uint16_t rot_b) {
    for (uint16_t offset = 0; offset < len; offset++) {
        const struct lock_chk_cycle_hop *hop_a = &hops[(rot_a + offset) % len];
        const struct lock_chk_cycle_hop *hop_b = &hops[(rot_b + offset) % len];
        int c = hops_cmp(hop_a, hop_b);
        if (c != 0)
            return c;
    }
    return 0;
}

uint64_t lock_chk_calc_sig(const struct lock_chk_cycle_hop *hops,
                           uint16_t cycle_len) {
    if (cycle_len == 0)
        return 0;

    uint16_t best_rot = 0;
    for (uint16_t i = 1; i < cycle_len; i++) {
        if (compare_rotations(hops, cycle_len, i, best_rot) < 0)
            best_rot = i;
    }

    uint64_t signature = HASH_FNV1A_64_OFFSET_BASIS;
    for (uint16_t step = 0; step < cycle_len; step++) {
        const struct lock_chk_cycle_hop *hop =
            &hops[(best_rot + step) % cycle_len];
        const char *from_name = hop->from_class ? hop->from_class->name : "";
        const char *from_file = hop->from_class ? hop->from_class->file : "";
        uint32_t from_line = hop->from_class ? hop->from_class->line : 0;
        const char *to_name = hop->to_class ? hop->to_class->name : "";
        const char *to_file = hop->to_class ? hop->to_class->file : "";
        uint32_t to_line = hop->to_class ? hop->to_class->line : 0;

        signature = lock_chk_hash_str(signature, from_name);
        signature = lock_chk_hash_str(signature, from_file);
        signature =
            lock_chk_hash_bytes(signature, &from_line, sizeof(from_line));
        signature = lock_chk_hash_bytes(signature, &hop->from_subclass,
                                        sizeof(hop->from_subclass));
        signature = lock_chk_hash_bytes(signature, &hop->from_mode,
                                        sizeof(hop->from_mode));
        signature = lock_chk_hash_str(signature, to_name);
        signature = lock_chk_hash_str(signature, to_file);
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

enum lock_chk_result lock_chk_graph_resolve_node(const struct lock_chk_ctx *ctx,
                                                 struct lock_chk_map *map,
                                                 uint8_t subclass,
                                                 struct lock_chk_node **out) {
    bool irqs_enabled = raw_spin_lock_high(&ctx->graph->lock);
    enum lock_chk_result result =
        resolve_node(ctx->graph, map, subclass, ctx->req, out);
    raw_spin_unlock_irq_restore(&ctx->graph->lock, irqs_enabled);
    return result;
}

static void make_cycle_fail(const struct lock_chk_ctx *ctx,
                            struct lock_chk_dep from, struct lock_chk_dep to) {
    *ctx->fault = (struct lock_chk_fault){
        .kind = LOCK_CHK_FAIL_CYCLE,
        .site = ctx->site,
        .class = to.node->class,
        .subclass = to.node->subclass,
        .mode = to.mode,
        .report = lock_chk_report_claim(),
    };

    struct lock_chk_report *report = ctx->fault->report;
    if (report == NULL)
        return;

    snprintf(report->msg, sizeof(report->msg),
             "Dependency cycle detected (%s -> %s)",
             from.node->class ? from.node->class->name : "lock",
             to.node->class ? to.node->class->name : "lock");
    report->cycle_len =
        extract_cycle(ctx->graph, from, to, ctx->site, report->cycle_hops,
                      LOCK_CHK_MAX_CYCLE_HOPS, &report->cycle_truncated);
    report->signature =
        lock_chk_calc_sig(report->cycle_hops, report->cycle_len);
}

static void make_cap_fail(const struct lock_chk_ctx *ctx,
                          struct lock_chk_node *to) {
    *ctx->fault = (struct lock_chk_fault){
        .kind = LOCK_CHK_FAIL_CAPACITY,
        .site = ctx->site,
        .class = to->class,
        .subclass = to->subclass,
        .capacity_pool = "edges",
        .capacity_used = ctx->graph->edge_count,
        .capacity_limit = LOCK_CHK_MAX_EDGES,
        .report = lock_chk_report_claim(),
    };

    if (ctx->fault->report != NULL)
        snprintf(ctx->fault->report->msg, sizeof(ctx->fault->report->msg),
                 "Edge capacity exhausted (%u/%u)", ctx->graph->edge_count,
                 LOCK_CHK_MAX_EDGES);
}

enum lock_chk_result lock_chk_graph_add_dep(const struct lock_chk_ctx *ctx,
                                            struct lock_chk_dep from,
                                            struct lock_chk_dep to) {
    bool irqs_enabled = raw_spin_lock_high(&ctx->graph->lock);
    enum lock_chk_result result = LOCK_CHK_RESULT_OK;

    if (find_edge(from, to) != NULL)
        goto out;

    if (graph_path(ctx->graph, to, from)) {
        if (ctx->fault != NULL)
            make_cycle_fail(ctx, from, to);
        result = LOCK_CHK_RESULT_CYCLE;
        goto out;
    }

    if (ctx->graph->edge_count == LOCK_CHK_MAX_EDGES) {
        if (ctx->fault != NULL)
            make_cap_fail(ctx, to.node);
        result = LOCK_CHK_RESULT_EDGE_CAPACITY;
        goto out;
    }

    publish_edge(ctx->graph, from, to, ctx->site);

out:
    raw_spin_unlock_irq_restore(&ctx->graph->lock, irqs_enabled);
    return result;
}

static bool dep_repeated(const struct lock_chk_thread_data *thread_data,
                         uint8_t index) {
    const struct lock_chk_held *candidate = &thread_data->held[index];

    for (uint8_t i = 0; i < index; i++) {
        const struct lock_chk_held *prior = &thread_data->held[i];
        if ((prior->lock.flags & LOCK_CHKD_ORDER) != 0 &&
            prior->node == candidate->node && prior->mode == candidate->mode)
            return true;
    }

    return false;
}

static struct lock_chk_dep held_dep(const struct lock_chk_held *held) {
    return (struct lock_chk_dep){.node = held->node, .mode = held->mode};
}

static bool held_contribs(const struct lock_chk_thread_data *td,
                          uint8_t index) {
    return (td->held[index].lock.flags & LOCK_CHKD_ORDER) != 0 &&
           !dep_repeated(td, index);
}

static enum lock_chk_result add_held_deps(const struct lock_chk_ctx *ctx,
                                          struct lock_chk_dep to) {
    const struct lock_chk_thread_data *thread_data = ctx->thread_data;
    size_t missing = 0;

    for (uint8_t i = 0; i < thread_data->depth; i++) {
        if (!held_contribs(thread_data, i))
            continue;

        struct lock_chk_dep from = held_dep(&thread_data->held[i]);
        if (find_edge(from, to) != NULL)
            continue;

        if (graph_path(ctx->graph, to, from)) {
            if (ctx->fault != NULL)
                make_cycle_fail(ctx, from, to);
            return LOCK_CHK_RESULT_CYCLE;
        }
        missing++;
    }

    if (missing > (size_t) (LOCK_CHK_MAX_EDGES - ctx->graph->edge_count)) {
        if (ctx->fault != NULL)
            make_cap_fail(ctx, to.node);
        return LOCK_CHK_RESULT_EDGE_CAPACITY;
    }

    for (uint8_t i = 0; i < thread_data->depth; i++) {
        if (!held_contribs(thread_data, i))
            continue;

        struct lock_chk_dep from = held_dep(&thread_data->held[i]);
        if (find_edge(from, to) != NULL)
            continue;

        publish_edge(ctx->graph, from, to, ctx->site);
    }

    return LOCK_CHK_RESULT_OK;
}

static void rollback_nodes(struct lock_chk_graph *graph,
                           uint16_t old_node_count) {
    while (graph->node_count > old_node_count) {
        struct lock_chk_node *node = &graph->nodes[--graph->node_count];
        hlist_del(&node->hash_entry);
    }
}

enum lock_chk_result lock_chk_graph_prepare_acq(const struct lock_chk_ctx *ctx,
                                                struct lock_chk_map *map,
                                                uint8_t subclass,
                                                struct lock_chk_node **out) {
    struct lock_chk_graph *graph = ctx->graph;
    const struct lock_chk_acq_req *request = ctx->req;
    bool irqs_enabled = raw_spin_lock_high(&graph->lock);
    uint16_t old_node_count = graph->node_count;
    struct lock_chk_node *old_base = atomic_load_rlx(&map->base_node);
    const struct lock_chk_class *class = lock_chk_map_class(map);
    struct lock_chk_node *existing_base = find_node(graph, class, 0);
    uint8_t old_context_bits =
        existing_base != NULL ? existing_base->context_bits : 0;

    enum lock_chk_result result =
        resolve_node(graph, map, subclass, request, out);
    if (result != LOCK_CHK_RESULT_OK)
        goto rollback;

    if ((request->op_flags & LOCK_OP_KIND_MASK) == LOCK_OP_KIND_BLOCKING) {
        result = add_held_deps(
            ctx, (struct lock_chk_dep){.node = *out, .mode = request->mode});
        if (result != LOCK_CHK_RESULT_OK)
            goto rollback;
    }

    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return LOCK_CHK_RESULT_OK;

rollback:
    rollback_nodes(graph, old_node_count);
    if (existing_base != NULL)
        existing_base->context_bits = old_context_bits;
    atomic_store_rlx(&map->base_node, old_base);
    *out = NULL;
    raw_spin_unlock_irq_restore(&graph->lock, irqs_enabled);
    return result;
}

#endif /* DEBUG_LOCK_CHK */
