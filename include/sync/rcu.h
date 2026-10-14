/* @title: RCU */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <tsa.h>

struct thread;

/* Forward declaration so we can put this pointer in threads */
struct rcu_node;

struct rcu_cb;
typedef void (*rcu_fn)(struct rcu_cb *);

struct rcu_cb {
    struct list_head list;
    rcu_fn fn;

    /* TODO: as debug flags get better, move away from raw TEST_ENABLED */
#ifdef TEST_ENABLED
    size_t gen_when_called; /* Diagnostics */
    size_t enqueued_waiting_on_gen;
    size_t target_gen;
#endif
};
#define rcu_cb_from_list_node(ln) (container_of(ln, struct rcu_cb, list))

TSA_REENTRANT_CAPABILITY_DECLARE("rcu", RCU_READ_LOCKED);

void rcu_init(void);

void rcu_read_lock(void) TSA_ACQUIRES(RCU_READ_LOCKED);
void rcu_read_unlock(void) TSA_RELEASES(RCU_READ_LOCKED);

void rcu_synchronize(void);

void rcu_defer(struct rcu_cb *cb, rcu_fn func);

void rcu_note_context_switch(struct thread *outgoing);
void rcu_note_irq_exit(void);

void rcu_idle_enter(void);
void rcu_idle_exit(void);

#define rcu_plain_t(p) __typeof_unqual__(*(p)) *

#define rcu_dereference(p)                                                     \
    ((rcu_plain_t(p)) atomic_load_acq((_Atomic __typeof_unqual__(p) *) &(p)))

#define rcu_assign_pointer(p, v)                                               \
    ({                                                                         \
        rcu_plain_t(p) _v = (v);                                               \
        atomic_store_release((_Atomic __typeof_unqual__(p) *) &(p),            \
                             (__typeof_unqual__(p)) _v);                       \
        _v;                                                                    \
    })

#define rcu_access_pointer(p) ((rcu_plain_t(p)) ca_read_once(p))
#define RCU_INIT_POINTER(p, v) ((p) = (typeof(p)) (rcu_plain_t(p))(v))

static inline cc_always_inline
cc_maybe_unused void rcu_read_guard_exit(int *unused)
    TSA_RELEASES(RCU_READ_LOCKED) TSA_NO_ANALYSIS {
    cc_unused(unused);
    rcu_read_unlock();
}

static inline cc_always_inline cc_maybe_unused int rcu_read_guard_enter(void)
    TSA_ACQUIRES(RCU_READ_LOCKED) TSA_NO_ANALYSIS {
    rcu_read_lock();
    return 0;
}

#define rcu_read_guard()                                                       \
    cc_cleanup(rcu_read_guard_exit) int PP_CONCAT(                             \
        rcu_read_guard_, __COUNTER__) = rcu_read_guard_enter()
