#pragma once

#ifdef DEBUG_LOCK_CHK

#include <atomic.h>
#include <math/hash.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stringify.h>
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
    LOCK_CHK_FAIL_NOT_HELD,
    LOCK_CHK_FAIL_UNEXPECTED_HELD,
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

#define LOCK_CHK_MSG_MAX 128

/* This is the actual contents of the message of an error,
 * and we only keep one in lock_chk_global */
struct lock_chk_report {
    uint64_t signature;
    uint16_t cycle_len;
    bool cycle_truncated;
    struct lock_chk_cycle_hop cycle_hops[LOCK_CHK_MAX_CYCLE_HOPS];
    char msg[LOCK_CHK_MSG_MAX];
};

/* *what* the failure actually is, which gets
 * passed by pointer and just on the stack */
struct lock_chk_fault {
    enum lock_chk_failure_kind kind;
    const struct lock_chk_class *class;
    const struct lock_chk_site *site;
    void *instance;
    struct lock_chk_report *report;
    const char *capacity_pool;
    uint32_t capacity_used;
    uint32_t capacity_limit;
    enum lock_chk_type type;
    enum lock_chk_mode mode;
    uint8_t subclass;
};

struct lock_chk_globals {
    struct lock_chk_graph graph;

    atomic(enum lock_chk_engine_state) state;
    atomic(enum lock_chk_engine_state) deep;
    atomic(enum lock_chk_engine_state) debug;

    bool panic_on_exhaustion;

    /* for claiming the report */
    atomic_bool report_busy;
    struct lock_chk_report report;
};

extern struct lock_chk_globals lock_chk_global;

struct lock_chk_dep {
    struct lock_chk_node *node;
    enum lock_chk_mode mode;
};

/* Things graph operations often need, bundled together. We keep
 * *graph in here so in the future we could potentially scale,
 * and right now, unit tests can test their own graphs */
struct lock_chk_ctx {
    struct lock_chk_graph *graph;
    const struct lock_chk_acq_req *req;
    const struct lock_chk_thread_data *thread_data;
    const struct lock_chk_site *site;
    struct lock_chk_fault *fault;
};

struct lock_debug_spin_entry {
    void *instance;
    const struct lock_chk_site *acquire_site;
    enum irql prev_irql;
    enum lock_chk_type type;
};

struct lock_debug_cpu {
    struct lock_debug_spin_entry stack[LOCK_CHK_MAX_SPIN_DEPTH];
    uint8_t depth;
};

struct lock_chk_guard {
    uint8_t *depth;
    bool irqs_enabled;
};

static inline struct lock_chk_dep lock_chk_dep_make(struct lock_chk_node *node,
                                                    enum lock_chk_mode mode) {
    return (struct lock_chk_dep) {.node = node, .mode = mode};
}

void lock_chk_graph_init(struct lock_chk_graph *graph);
uint64_t lock_chk_calc_sig(const struct lock_chk_cycle_hop *hops,
                           uint16_t cycle_len);
void lock_chk_report_fail(const struct lock_chk_fault *fault);
enum lock_chk_result lock_chk_graph_resolve_node(const struct lock_chk_ctx *ctx,
                                                 struct lock_chk_map *map,
                                                 uint8_t subclass,
                                                 struct lock_chk_node **out);
enum lock_chk_result lock_chk_graph_add_dep(const struct lock_chk_ctx *ctx,
                                            struct lock_chk_dep from,
                                            struct lock_chk_dep to);
enum lock_chk_result lock_chk_graph_prepare_acq(const struct lock_chk_ctx *ctx,
                                                struct lock_chk_map *map,
                                                uint8_t subclass,
                                                struct lock_chk_node **out);

void lock_chk_deep_activate(void);
void lock_debug_activate(void);

void lock_chk_before_acq(struct lock_chk_acq_token *token,
                         const struct lock_chk_acq_req *request);
void lock_chk_acqd(struct lock_chk_acq_token *token);
void lock_chk_cancel(struct lock_chk_acq_token *token);
void lock_chk_before_rel(struct lock_chk_rel_token *token,
                         const struct lock_chk_rel_req *request);
void lock_chk_reld(struct lock_chk_rel_token *token);

struct lock_chk_guard lock_chk_enter(void) TSA_ACQUIRES_IRQS;
void lock_chk_leave(const struct lock_chk_guard *guard) TSA_RELEASES_IRQS;

void lock_chk_thread_init(struct thread *thread);
void lock_chk_thread_exit(struct thread *thread);

static inline bool lock_chk_modes_conflict(enum lock_chk_mode a,
                                           enum lock_chk_mode b) {
    return a == LOCK_CHK_MODE_EXCLUSIVE || b == LOCK_CHK_MODE_EXCLUSIVE;
}

static inline struct lock_chk_report *lock_chk_report_claim(void) {
    if (atomic_xchg_acq_rel(&lock_chk_global.report_busy, true))
        return NULL;

    struct lock_chk_report *report = &lock_chk_global.report;
    report->signature = 0;
    report->cycle_len = 0;
    report->cycle_truncated = false;
    report->msg[0] = '\0';
    return report;
}

static inline void lock_chk_report_release(void) {
    atomic_store_release(&lock_chk_global.report_busy, false);
}

static inline void lock_chk_fail(struct lock_chk_fault *fault, const char *fmt,
                                 ...) {
    fault->report = lock_chk_report_claim();
    if (fault->report != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(fault->report->msg, sizeof(fault->report->msg), fmt, args);
        va_end(args);
    }

    lock_chk_report_fail(fault);
}

static inline struct lock_chk_fault
lock_chk_fault_from_id(enum lock_chk_failure_kind kind, struct lock_chk_id lock,
                       const struct lock_chk_map *map,
                       const struct lock_chk_site *site,
                       enum lock_chk_mode mode, uint8_t subclass) {
    return (struct lock_chk_fault) {
        .kind = kind,
        .class = map != NULL ? map->class : NULL,
        .site = site,
        .instance = lock.instance,
        .type = lock.type,
        .mode = mode,
        .subclass = subclass,
    };
}

static inline struct lock_chk_fault
lock_chk_fault_from_req(enum lock_chk_failure_kind kind,
                        const struct lock_chk_acq_req *request) {
    return lock_chk_fault_from_id(kind, request->lock, request->map,
                                  request->site, request->mode,
                                  request->subclass);
}

static inline struct lock_chk_fault
lock_chk_fault_from_rel(enum lock_chk_failure_kind kind,
                        const struct lock_chk_rel_req *request) {
    return lock_chk_fault_from_id(kind, request->lock, request->map,
                                  request->site, request->mode, 0);
}

/* For paths holding the live lock itself */
static inline struct lock_chk_fault lock_chk_fault_from_lock(
    enum lock_chk_failure_kind kind, const struct lock_chk_lock *lock,
    const struct lock_chk_site *site, enum lock_chk_mode mode) {
    return lock_chk_fault_from_id(kind, lock_chk_id_of(lock), &lock->map, site,
                                  mode, 0);
}

static inline uint64_t lock_chk_hash_bytes(uint64_t hash, const void *data,
                                           size_t len) {
    return hash_fnv1a_64_update(hash, data, len);
}

static inline uint64_t lock_chk_hash_str(uint64_t hash, const char *str) {
    const char *value = str != NULL ? str : "";
    return lock_chk_hash_bytes(hash, value, strlen(value));
}

static inline bool
lock_chk_state_active(const atomic(enum lock_chk_engine_state) * state) {
    return atomic_load_acq(state) == LOCK_CHK_ACTIVE;
}

static inline const struct lock_chk_class *
lock_chk_map_class(const struct lock_chk_map *map) {
    return map->class != NULL ? map->class : &map->instance_class;
}

static inline bool lock_chk_type_is_spin(enum lock_chk_type type) {
    return type == LOCK_CHK_TYPE_SPIN || type == LOCK_CHK_TYPE_QSPIN;
}

static inline bool lock_chk_type_is_blocking(enum lock_chk_type type) {
    return type == LOCK_CHK_TYPE_MUTEX || type == LOCK_CHK_TYPE_MUTEX_SIMPLE ||
           type == LOCK_CHK_TYPE_RWLOCK;
}

static inline struct lock_chk_held *
lock_chk_find_held(struct lock_chk_thread_data *thread_data,
                   const void *instance, enum lock_chk_type type,
                   bool match_type, enum lock_chk_mode mode, bool match_mode) {
    for (uint8_t i = 0; i < thread_data->depth; i++) {
        struct lock_chk_held *held = &thread_data->held[i];
        if (held->lock.instance != instance)
            continue;

        if (match_type && held->lock.type != type)
            continue;

        if (match_mode && held->mode != mode)
            continue;

        return held;
    }

    return NULL;
}

static inline bool lock_op_irq_safe(enum lock_op_flags flags) {
    return (flags & LOCK_OP_IRQ_MASK) == LOCK_OP_IRQ_HIGH;
}

static inline size_t lock_chk_class_hash(const struct lock_chk_class *class,
                                         uint8_t subclass) {
    uintptr_t key = (uintptr_t) class;
    return ((key >> 4) ^ (key >> 13) ^ subclass) % LOCK_CHK_HASH_BUCKETS;
}

static inline uint16_t lock_chk_mode_bit(enum lock_chk_mode mode) {
    enforce(mode == LOCK_CHK_MODE_SHARED || mode == LOCK_CHK_MODE_EXCLUSIVE);
    return mode == LOCK_CHK_MODE_EXCLUSIVE ? 1 : 0;
}

static inline uint16_t lock_chk_mode_state(const struct lock_chk_node *node,
                                           enum lock_chk_mode mode) {
    return (uint16_t) (node->id * 2 + lock_chk_mode_bit(mode));
}

static inline struct lock_chk_node *
lock_chk_state_node(struct lock_chk_graph *graph, uint16_t state) {
    return &graph->nodes[state / 2];
}

static inline enum lock_chk_mode lock_chk_state_mode(uint16_t state) {
    return (state % 2) != 0 ? LOCK_CHK_MODE_EXCLUSIVE : LOCK_CHK_MODE_SHARED;
}

STRINGIFY_DECLARE_ENUM(lock_chk_type) {
    switch (stringify_in) {
    case LOCK_CHK_TYPE_SPIN: return "spinlock";
    case LOCK_CHK_TYPE_QSPIN: return "qspinlock";
    case LOCK_CHK_TYPE_MUTEX: return "mutex";
    case LOCK_CHK_TYPE_MUTEX_SIMPLE: return "simple mutex";
    case LOCK_CHK_TYPE_RWLOCK: return "rwlock";
    default: cc_unreachable();
    }
}

STRINGIFY_DECLARE(lock_chk_fail_kind_to_str, enum lock_chk_failure_kind) {
    switch (stringify_in) {
    case LOCK_CHK_FAIL_CYCLE: return "cycle";
    case LOCK_CHK_FAIL_RECURSION: return "recursion";
    case LOCK_CHK_FAIL_RELEASE: return "release";
    case LOCK_CHK_FAIL_CONTEXT: return "context";
    case LOCK_CHK_FAIL_THREAD_EXIT: return "thread_exit";
    case LOCK_CHK_FAIL_SPIN_ORDER: return "spin_order";
    case LOCK_CHK_FAIL_CAPACITY: return "capacity";
    case LOCK_CHK_FAIL_UNINITIALIZED: return "uninitialized";
    case LOCK_CHK_FAIL_NOT_HELD: return "not_held";
    case LOCK_CHK_FAIL_UNEXPECTED_HELD: return "unexpected_held";
    default: cc_unreachable();
    }
}

#endif /* DEBUG_LOCK_CHK */
