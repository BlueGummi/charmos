#pragma once

#ifdef DEBUG_LOCK_CHK

#include <math/hash.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <structures/hlist.h>
#include <structures/list.h>
#include <sync/lock_chk_types.h>
#include <sync/raw_spinlock.h>

struct thread;

enum lock_chk_result : uint8_t {
    LOCK_CHK_RESULT_OK,
    LOCK_CHK_RESULT_INACTIVE,
    LOCK_CHK_RESULT_RECURSION,
    LOCK_CHK_RESULT_CYCLE,
    LOCK_CHK_RESULT_NOT_HELD,
    LOCK_CHK_RESULT_WRONG_THREAD,
    LOCK_CHK_RESULT_NODE_CAPACITY,
    LOCK_CHK_RESULT_EDGE_CAPACITY,
    LOCK_CHK_RESULT_HELD_CAPACITY,
    LOCK_CHK_RESULT_BAD_CONTEXT,
    LOCK_CHK_RESULT_INTERNAL,
};

struct lock_chk_node {
    struct hlist_node hash_entry;
    struct list_head out_edges;
    struct list_head in_edges;
    const struct lock_chk_class *class;
    uint16_t id;
    uint8_t subclass;
    uint8_t context_bits;
};

struct lock_chk_edge {
    struct list_head from_entry;
    struct list_head to_entry;
    struct lock_chk_node *from;
    struct lock_chk_node *to;
    const struct lock_chk_site *first_site;
    enum lock_chk_mode from_mode;
    enum lock_chk_mode to_mode;
};

struct lock_chk_graph {
    struct raw_spinlock lock;
    struct hlist_head class_hash[LOCK_CHK_HASH_BUCKETS];
    struct lock_chk_node nodes[LOCK_CHK_MAX_NODES];
    struct lock_chk_edge edges[LOCK_CHK_MAX_EDGES];
    uint16_t node_count;
    uint16_t edge_count;
    uint32_t visit_generation[LOCK_CHK_MODE_STATE_COUNT];
    int32_t parent_state[LOCK_CHK_MODE_STATE_COUNT];
    int32_t parent_edge[LOCK_CHK_MODE_STATE_COUNT];
    uint16_t dfs_stack[LOCK_CHK_MODE_STATE_COUNT];
    int32_t last_cycle_end_state;
    uint32_t generation;
};

enum lock_chk_failure_kind : uint8_t {
    LOCK_CHK_FAIL_CYCLE,
    LOCK_CHK_FAIL_RECURSION,
    LOCK_CHK_FAIL_RELEASE,
    LOCK_CHK_FAIL_CONTEXT,
    LOCK_CHK_FAIL_THREAD_EXIT,
    LOCK_CHK_FAIL_SPIN_ORDER,
    LOCK_CHK_FAIL_CAPACITY,
    LOCK_CHK_FAIL_UNINITIALIZED,
};

#define LOCK_CHK_MAX_CYCLE_HOPS 16

struct lock_chk_cycle_hop {
    const struct lock_chk_class *from_class;
    const struct lock_chk_class *to_class;
    const struct lock_chk_site *site;
    uint8_t from_subclass;
    uint8_t to_subclass;
    enum lock_chk_mode from_mode;
    enum lock_chk_mode to_mode;
};

struct lock_chk_failure {
    enum lock_chk_failure_kind kind;
    const struct lock_chk_site *site;
    const struct lock_chk_class *class;
    void *instance;
    enum lock_chk_type type;
    enum lock_chk_mode mode;
    uint8_t subclass;
    uint16_t cycle_len;
    bool cycle_truncated;
    uint64_t signature;
    struct lock_chk_cycle_hop cycle_hops[LOCK_CHK_MAX_CYCLE_HOPS];
    const char *capacity_pool;
    uint32_t capacity_used;
    uint32_t capacity_limit;
    char msg[128];
};

void lock_chk_graph_init(struct lock_chk_graph *graph);
uint16_t lock_chk_graph_extract_cycle_locked(
    struct lock_chk_graph *graph, struct lock_chk_node *from,
    enum lock_chk_mode from_mode, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_cycle_hop *hops, uint16_t max_hops, bool *truncated);
uint64_t
lock_chk_calc_canonical_cycle_sig(const struct lock_chk_cycle_hop *hops,
                                  uint16_t cycle_len);
void lock_chk_report_failure(const struct lock_chk_failure *failure);
enum lock_chk_result lock_chk_graph_resolve_node(
    struct lock_chk_graph *graph, struct lock_chk_map *map, uint8_t subclass,
    const struct lock_chk_acquire_request *request, struct lock_chk_node **out);
enum lock_chk_result lock_chk_graph_add_dependency(
    struct lock_chk_graph *graph, struct lock_chk_node *from,
    enum lock_chk_mode from_mode, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_failure *failure_out);
enum lock_chk_result lock_chk_graph_add_held_dependencies(
    struct lock_chk_graph *graph,
    const struct lock_chk_thread_data *thread_data, struct lock_chk_node *to,
    enum lock_chk_mode to_mode, const struct lock_chk_site *site,
    struct lock_chk_failure *failure_out);
enum lock_chk_result lock_chk_graph_prepare_acquire(
    struct lock_chk_graph *graph, struct lock_chk_map *map, uint8_t subclass,
    const struct lock_chk_acquire_request *request,
    const struct lock_chk_thread_data *thread_data, struct lock_chk_node **out,
    struct lock_chk_failure *failure_out);

void lock_chk_deep_activate(void);
void lock_debug_activate(void);
bool lock_chk_capacity_should_panic(void);

void lock_chk_before_acquire(struct lock_chk_acquire_token *token,
                             const struct lock_chk_acquire_request *request);
void lock_chk_acquired(struct lock_chk_acquire_token *token);
void lock_chk_cancel(struct lock_chk_acquire_token *token);
void lock_chk_before_release(struct lock_chk_release_token *token,
                             const struct lock_chk_release_request *request);
void lock_chk_released(struct lock_chk_release_token *token);

void lock_chk_thread_init(struct thread *thread);
void lock_chk_thread_exit(struct thread *thread);

static inline bool lock_chk_modes_conflict(enum lock_chk_mode a,
                                           enum lock_chk_mode b) {
    return a == LOCK_CHK_MODE_EXCLUSIVE || b == LOCK_CHK_MODE_EXCLUSIVE;
}

static inline void lock_chk_fail(struct lock_chk_failure *fail, const char *fmt,
                                 ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(fail->msg, sizeof(fail->msg), fmt, args);
    va_end(args);
    lock_chk_report_failure(fail);
}

static inline uint64_t lock_chk_hash_bytes(uint64_t hash, const void *data,
                                           size_t len) {
    return hash_fnv1a_64_update(hash, data, len);
}

static inline uint64_t lock_chk_hash_string(uint64_t hash, const char *str) {
    const char *value = str != NULL ? str : "";
    return lock_chk_hash_bytes(hash, value, strlen(value));
}

#endif /* DEBUG_LOCK_CHK */
