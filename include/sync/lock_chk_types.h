/* @title: Lock Validation Layout Types */
#pragma once
#include <asm.h>
#include <irq/irq.h>
#include <sch/irql.h>
#include <stdatomic.h>
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
    LOCK_CHK_CTX_THREAD = 1 << 0,
    LOCK_CHK_CTX_IRQ = 1 << 1,
    LOCK_CHK_CTX_SPIN_DISPATCH = 1 << 2,
    LOCK_CHK_CTX_SPIN_HIGH = 1 << 3,
    LOCK_CHK_CTX_RAW_OPERATION = 1 << 4,
};

#ifdef DEBUG_LOCK_CHK

struct lock_chk_map {
    const struct lock_chk_class *class;
    struct lock_chk_class instance_class;
    _Atomic(struct lock_chk_node *) base_node;
};

struct lock_chk_held {
    struct lock_chk_node *node;
    void *instance;
    const struct lock_chk_site *acquire_site;
    uint64_t acquire_tsc;
    enum irql prev_irql;
    cpu_id_t cpu;
    enum lock_chk_flags flags;
    enum lock_chk_type type;
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

struct lock_chk_acquire_request {
    struct lock_chk_map *map;
    void *instance;
    const struct lock_chk_site *site;
    enum lock_chk_flags flags;
    enum lock_chk_type type;
    enum lock_chk_mode mode;
    enum lock_chk_wait_kind wait_kind;
    enum irql prev_irql;
    uint8_t subclass;
    bool raw_operation;
    bool irq_safe;
    bool irqs_enabled;
    bool in_irq;
    bool in_nmi;
};

struct lock_chk_acquire_token {
    struct lock_chk_node *node;
    struct lock_chk_node *context_node;
    const struct lock_chk_acquire_request *request;
    struct lock_chk_thread_data *thread_data;
    bool active;
};

struct lock_chk_release_request {
    struct lock_chk_map *map;
    void *instance;
    const struct lock_chk_site *site;
    enum lock_chk_flags flags;
    enum lock_chk_type type;
    enum lock_chk_mode mode;
};

struct lock_chk_release_token {
    struct lock_chk_thread_data *thread_data;
    void *instance;
    uint8_t held_index;
    bool active;
};

void lock_chk_before_acquire(struct lock_chk_acquire_token *token,
                             const struct lock_chk_acquire_request *request);
void lock_chk_acquired(struct lock_chk_acquire_token *token);
void lock_chk_cancel(struct lock_chk_acquire_token *token);
void lock_chk_before_release(struct lock_chk_release_token *token,
                             const struct lock_chk_release_request *request);
void lock_chk_released(struct lock_chk_release_token *token);

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

static inline void
lock_chk_map_runtime_init(struct lock_chk_map *map,
                          const struct lock_chk_class *class) {
    map->class = class;
    map->instance_class = (struct lock_chk_class) {
        .name = "<instance>",
        .file = __RELFILE__,
        .line = 0,
    };
    atomic_store_explicit(&map->base_node, NULL, memory_order_relaxed);
}

static inline struct lock_chk_acquire_request lock_chk_acquire_request_make(
    struct lock_chk_map *map, void *instance, const struct lock_chk_site *site,
    enum lock_chk_flags flags, enum lock_chk_type type, enum lock_chk_mode mode,
    enum lock_chk_wait_kind wait_kind, unsigned int subclass,
    bool raw_operation, bool irq_safe) {
    return (struct lock_chk_acquire_request) {
        .map = map,
        .instance = instance,
        .site = site,
        .flags = flags,
        .type = type,
        .mode = mode,
        .wait_kind = wait_kind,
        .prev_irql = irql_get(),
        .subclass = (uint8_t) subclass,
        .raw_operation = raw_operation,
        .irq_safe = irq_safe,
        .irqs_enabled = are_interrupts_enabled(),
        .in_irq = irq_in_interrupt(),
        .in_nmi = irq_in_nmi(),
    };
}

static inline struct lock_chk_release_request lock_chk_release_request_make(
    struct lock_chk_map *map, void *instance, const struct lock_chk_site *site,
    enum lock_chk_flags flags, enum lock_chk_type type,
    enum lock_chk_mode mode) {
    return (struct lock_chk_release_request) {
        .map = map,
        .instance = instance,
        .site = site,
        .flags = flags,
        .type = type,
        .mode = mode,
    };
}

#endif /* DEBUG_LOCK_CHK */
