/* @title: Lock Validation Layout Types */
#pragma once
#include <asm.h>
#include <atomic.h>
#include <irq/irq.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/lock_chk.h>
#include <types/types.h>

struct lock_chk_node;

#define LOCK_CHK_MAX_NODES 1024
#define LOCK_CHK_MAX_EDGES 8192
#define LOCK_CHK_HASH_BUCKETS 1024
#define LOCK_CHK_MAX_HELD_LOCKS 32
#define LOCK_CHK_MAX_SPIN_DEPTH 32
#define LOCK_CHK_MAX_SUBCLASSES 8
#define LOCK_CHK_MODE_STATE_COUNT (LOCK_CHK_MAX_NODES * 2)

enum lock_chk_context_bits : uint8_t {
    LOCK_CHK_CTX_IRQ = 1 << 0,
    LOCK_CHK_CTX_SPIN_DISPATCH = 1 << 1,
    LOCK_CHK_CTX_SPIN_HIGH = 1 << 2,
    LOCK_CHK_CTX_RAW_OPERATION = 1 << 3,
};

#ifdef DEBUG_LOCK_CHK

struct lock_chk_map {
    const struct lock_chk_class *class;
    struct lock_chk_class instance_class;
    atomic(struct lock_chk_node *) base_node;
};

struct lock_chk_lock {
    atomic_bool used;
    struct lock_chk_map map;
    void *instance;

    enum lock_chk_type type : 4;
    enum lock_chk_flags flags : 4;
    bool initialized : 1;
    bool manages_irql : 1;
    bool raw_operation : 1;
};

struct lock_chk_id {
    void *instance;
    enum lock_chk_type type;
    enum lock_chk_flags flags;
};

static inline struct lock_chk_id
lock_chk_id_of(const struct lock_chk_lock *lock) {
    return (struct lock_chk_id) {
        .instance = lock->instance,
        .type = lock->type,
        .flags = lock->flags,
    };
}

struct lock_chk_held {
    struct lock_chk_node *node;
    struct lock_chk_id lock;
    const struct lock_chk_site *acquire_site;
    uint64_t acquire_tsc;
    enum irql prev_irql;
    uint16_t cpu; /* TODO: We use this to represent CPU due to size,
                   * cpu_id_t is a size_t */
    enum lock_chk_mode mode;
    uint8_t subclass;
    bool trylock;
    bool raw_operation;
};

struct lock_chk_thread_data {
    struct lock_chk_held held[LOCK_CHK_MAX_HELD_LOCKS];
    uint8_t depth;
    uint8_t thread_checked_depth;
    uint8_t thread_checked_spin_depth;
};

struct lock_chk_acq_req {
    struct lock_chk_map *map;
    struct lock_chk_id lock;
    const struct lock_chk_site *site;
    enum lock_chk_mode mode;
    enum irql prev_irql;
    uint8_t subclass;
    enum lock_op_flags op_flags;
    bool irqs_enabled : 1;
    bool in_irq : 1;
    bool in_nmi : 1;
};

struct lock_chk_acq_token {
    struct lock_chk_node *node;
    struct lock_chk_node *context_node;
    const struct lock_chk_acq_req *request;
    struct lock_chk_thread_data *thread_data;
    bool active;
};

struct lock_chk_rel_req {
    struct lock_chk_map *map;
    struct lock_chk_id lock;
    const struct lock_chk_site *site;
    enum lock_chk_mode mode;
};

struct lock_chk_rel_token {
    struct lock_chk_thread_data *thread_data;
    void *instance;
    uint8_t held_index;
    bool active;
};

void lock_chk_before_acq(struct lock_chk_acq_token *token,
                         const struct lock_chk_acq_req *request);
void lock_chk_acqd(struct lock_chk_acq_token *token);
void lock_chk_cancel(struct lock_chk_acq_token *token);
void lock_chk_before_rel(struct lock_chk_rel_token *token,
                         const struct lock_chk_rel_req *request);
void lock_chk_reld(struct lock_chk_rel_token *token);

/* LOCK_CHK_ASSERT_HELD/NOT_HELD and whatnot use this, which
 * returns true if the engine could evaluate the assertion,
 * and otherwise will return false, and panic if a violation is found */
bool lock_chk_assert_held_deep(struct lock_chk_lock *lock,
                               enum lock_chk_mode mode, bool want_held,
                               const struct lock_chk_site *site);

#define LOCK_CHK_MAP_VALUE_INIT(class_)                                        \
    ((struct lock_chk_map) {                                                   \
        .class = (class_),                                                     \
        .instance_class =                                                      \
            {                                                                  \
                .name = "<instance>",                                          \
                .file = __RELFILE__,                                           \
                .line = __LINE__,                                              \
            },                                                                 \
        .base_node = ATOMIC_VAR_INIT(NULL),                                    \
    })

#define LOCK_CHK_LOCK_VALUE_INIT(class_, flags_)                               \
    ((struct lock_chk_lock) {                                                  \
        .flags = (flags_),                                                     \
        .initialized = true,                                                   \
        .used = ATOMIC_VAR_INIT(false),                                        \
        .map = LOCK_CHK_MAP_VALUE_INIT(class_),                                \
    })

static inline void
lock_chk_map_runtime_init(struct lock_chk_map *map,
                          const struct lock_chk_class *class) {
    map->class = class;
    map->instance_class = (struct lock_chk_class) {
        .name = "<instance>",
        .file = __RELFILE__,
        .line = 0,
    };
    atomic_store_rlx(&map->base_node, NULL);
}

static inline struct lock_chk_acq_req
lock_chk_acq_req_make(struct lock_chk_lock *lock,
                      const struct lock_chk_site *site, enum lock_chk_mode mode,
                      uint8_t subclass, enum lock_op_flags flags) {
    return (struct lock_chk_acq_req) {
        .map = &lock->map,
        .lock = lock_chk_id_of(lock),
        .site = site,
        .mode = mode,
        .prev_irql = irql_get(),
        .subclass = subclass,
        .op_flags = flags,
        .irqs_enabled = irqs_enabled(),
        .in_irq = irq_in_interrupt(),
        .in_nmi = irq_in_nmi(),
    };
}

static inline struct lock_chk_rel_req
lock_chk_rel_req_make(struct lock_chk_lock *lock,
                      const struct lock_chk_site *site,
                      enum lock_chk_mode mode) {
    return (struct lock_chk_rel_req) {
        .map = &lock->map,
        .lock = lock_chk_id_of(lock),
        .site = site,
        .mode = mode,
    };
}

#endif /* DEBUG_LOCK_CHK */
