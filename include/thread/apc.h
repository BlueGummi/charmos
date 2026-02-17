/* @title: APCs */
#pragma once
#include <irq/irq.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <thread/apc_types.h>

/* Forward declarations :trl: */

struct apc {
    apc_func_t func;
    void *arg1;
    void *arg2;
    bool enqueued;
    atomic_bool cancelled;

    struct list_head list;

    struct thread *owner;
    struct apc_event_desc *desc; /* NULL if this is not an event APC */
    size_t execute_times;        /* for event apcs. if >= 1 it should
                                    be on to_exec_event_apcs */
};

struct apc_event_desc {
    const char *name; /* TODO: add fields to this structure */
};

#define APC_EVENT_SYMBOL_PREFIX_INTERNAL __apc_event_

#define APC_EVENT_EXTERN(n)                                                    \
    extern struct apc_event_desc APC_EVENT_SYMBOL_PREFIX_INTERNAL##n

#define APC_EVENT_CREATE(n, strname)                                           \
    struct apc_event_desc APC_EVENT_SYMBOL_PREFIX_INTERNAL##n = {.name =       \
                                                                     strname}
#define APC_EVENT(n) &(APC_EVENT_SYMBOL_PREFIX_INTERNAL##n)

static inline const char *apc_event_str(struct apc_event_desc *evt) {
    return evt->name;
}

struct apc *apc_create(void);
void apc_init(struct apc *a, apc_func_t fn, void *arg1, void *arg2);
void apc_event_signal(struct apc_event_desc *desc);
void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type);
void apc_enqueue_event_apc(struct apc *a, struct apc_event_desc *d);

static inline void apc_enqueue_on_curr(struct apc *a, enum apc_type type) {
    apc_enqueue(scheduler_get_current_thread(), a, type);
}

bool apc_cancel(struct apc *a);

void apc_check_and_deliver(struct thread *t);

void apc_enable_special();
void apc_disable_special();

void apc_enable_kernel();
void apc_disable_kernel();

void apc_free_on_thread(struct thread *t);
