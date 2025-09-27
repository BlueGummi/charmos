#include <int/irq.h>
#include <misc/list.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#pragma once

/* Forward declarations :trl: */
struct thread;
struct apc;

enum apc_type { APC_TYPE_SPECIAL_KERNEL, APC_TYPE_KERNEL, APC_TYPE_COUNT };

enum apc_event {
    APC_EVENT_THREAD_MIGRATE,
    APC_EVENT_THREAD_EXIT,
    APC_EVENT_COUNT,
    APC_EVENT_NONE, /* Nothing happened */
};

typedef void (*apc_func_t)(struct apc *apc, void *arg1, void *arg2);

struct apc {
    apc_func_t func;
    void *arg1;
    void *arg2;
    bool enqueued;
    atomic_bool cancelled;
    struct thread *owner;
    struct list_head node;
};

struct apc *apc_create(void);
void apc_init(struct apc *a, apc_func_t fn, void *arg1, void *arg2);
void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type);
void apc_enqueue_event_apc(struct thread *t, struct apc *a, enum apc_event evt);
void apc_enqueue_on_curr(struct apc *a, enum apc_type type);
bool apc_cancel(struct apc *a);

void thread_set_recent_apc_event(struct thread *t, enum apc_event event);
void thread_exec_apcs(struct thread *t);
void thread_exec_event_apcs(struct thread *t);
void thread_check_and_deliver_apcs(struct thread *t);

void thread_enable_special_apcs(struct thread *t);
void thread_disable_special_apcs(struct thread *t);

void thread_enable_kernel_apcs(struct thread *t);
void thread_disable_kernel_apcs(struct thread *t);

void thread_free_event_apcs(struct thread *t);

static inline bool safe_to_exec_apcs(void) {
    return irql_get() == IRQL_PASSIVE_LEVEL && irq_in_thread_context();
}
