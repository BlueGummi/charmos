/* @title: RCU */
#pragma once
#include <compiler.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>

struct thread;

/* Forward declaration so we can put this pointer in threads */
struct rcu_node;

struct rcu_cb;
typedef void (*rcu_fn)(struct rcu_cb *, void *);

struct rcu_cb {
    struct list_head list;
    rcu_fn fn;
    void *arg;

    size_t gen_when_called; /* Diagnostics */
    size_t enqueued_waiting_on_gen;
    size_t target_gen;
};
#define rcu_cb_from_list_node(ln) (container_of(ln, struct rcu_cb, list))

void rcu_init(void);

void rcu_read_lock(void);
void rcu_read_unlock(void);

void rcu_synchronize(void);
void rcu_defer(struct rcu_cb *cb, rcu_fn fn, void *arg);

/* TODO: next_is_idle is a little funny... perhaps it's better to explicitly
 * state *prev, *next here and just check the idle state inside */
void rcu_note_context_switch(struct thread *outgoing, bool next_is_idle);
void rcu_note_irq_exit(void);

#define rcu_dereference(p) atomic_load_explicit(&(p), memory_order_acquire)

#define rcu_assign_pointer(p, v)                                               \
    atomic_store_explicit(&(p), (v), memory_order_release)
