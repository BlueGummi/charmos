#pragma once
#include <compiler.h>
#include <log.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/cpu_mask.h>
#include <structures/list.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <types/types.h>

/* TODO: Here, we have 32 bit masks per node, with 4 levels, since right now
 * CPU_MASK has a maximum of 128 CPUs, however, in the future, we might
 * want to come up with something smarter + maybe introduce NR_CPUS */
#define RCU_FANOUT 32
#define RCU_MAX_LEVELS 4

/* Frequency for workers to poke CPUs that are not quiesced */
#define RCU_KICK_INTERVAL_MS 5

/* TODO: Tune this */
#define RCU_STALL_MS 1000

/* TODO: Tune this ? */
#define RCU_STALL_MAX_REPORTED 8

/* Tree node, everything except parent, child_index, and full_ is
 * protected by lock, always irq disabled on acquire, since IRQ
 * exit takes that lock,
 *
 * ordering leaf -> parent -> ... -> root, and node locks
 * get dropped before parent locks are taken
 *
 * TODO: We need to make this topology aware in the future, but for
 * this naive tree impl. we can cheese it with what we're doing here */
struct rcu_node {
    struct spinlock lock;

    struct rcu_node *parent;
    size_t child_index; /* our bit in parent->qs_children */

    uint64_t gp_seq;        /* GP this node is tracking */
    uint64_t completed_seq; /* last GP we propagated upward */

    bitmap_word_t full_children; /* every child we own */
    bitmap_word_t qs_children;   /* children that still owe a QS */

    bool is_leaf;
    cpu_id_t cpu_base;
    struct cpu_mask full_cpus; /* every CPU we own */
    struct cpu_mask qs_cpus;   /* CPUs that still owe a QS */
    struct cpu_mask idle_cpus; /* CPUs RCU sees are idle, i.e. quiescent */
    uint32_t blocked_count;    /* registered readers counted against gp_seq */
    struct list_head blocked;  /* threads preempted in a read section */
};

/* Per-CPU RCU data, so rcu_defer() doesn't have global contention
 * and reported_seq lets interrupt exit skip leaf lock */
struct rcu_cpu {
    struct spinlock lock;
    struct list_head list;
    _Atomic uint64_t reported_seq;
} __cache_aligned;

struct rcu_state {
    bool ready;

    _Atomic uint64_t gp_seq;       /* last GP started */
    _Atomic uint64_t gp_completed; /* last GP completed */
    _Atomic uint64_t gp_requests;  /* pending rcu_synchronize() requests */

    struct semaphore sem; /* wake the GP worker */

    struct rcu_node *nodes; /* root first */
    size_t node_count;
    struct rcu_node *root;
    struct rcu_node *leaves;
    size_t leaf_count;

    struct rcu_cpu *cpus; /* one per CPU */
    struct thread *worker;
};

struct rcu_stall_blocker {
    thread_id_t id;
    const char *name;
    int state;
    uint64_t read_seq;
};

LOG_SITE_EXTERN(rcu);
LOG_HANDLE_EXTERN(rcu);

#define rcu_log(lvl, fmt, ...)                                                 \
    log(LOG_SITE(rcu), LOG_HANDLE(rcu), lvl, fmt, ##__VA_ARGS__)

#define rcu_err(fmt, ...) rcu_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define rcu_warn(fmt, ...) rcu_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define rcu_info(fmt, ...) rcu_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define rcu_debug(fmt, ...) rcu_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define rcu_trace(fmt, ...) rcu_log(LOG_TRACE, fmt, ##__VA_ARGS__)
