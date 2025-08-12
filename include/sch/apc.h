#include <misc/list.h>
#include <mp/core.h>
#include <sch/thread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#pragma once

struct thread;

enum apc_type { APC_TYPE_SPECIAL_KERNEL, APC_TYPE_KERNEL, APC_TYPE_COUNT };

struct apc;
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

void apc_init(struct apc *a, apc_func_t fn, void *arg1, void *arg2);
void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type);
void apc_enqueue_on_curr(struct apc *a, enum apc_type type);
bool apc_cancel(struct apc *a);

void thread_exec_apcs(struct thread *t);
void thread_check_and_deliver_apcs(struct thread *t);

void thread_enable_special_apcs(struct thread *t);
void thread_disable_special_apcs(struct thread *t);

void thread_enable_kernel_apcs(struct thread *t);
void thread_disable_kernel_apcs(struct thread *t);

static inline bool safe_to_exec_apcs(void) {
    return get_irql() == IRQL_PASSIVE_LEVEL && in_thread_context();
}

static inline bool thread_has_apcs(struct thread *t) {
    return t->apc_pending_mask != 0;
}

#define APC_DECLARE(fn)                                                        \
    static struct apc fn##_apc = {.func = fn,                                  \
                                  .arg1 = NULL,                                \
                                  .arg2 = NULL,                                \
                                  .enqueued = false,                           \
                                  .cancelled = false,                          \
                                  .owner = NULL}
