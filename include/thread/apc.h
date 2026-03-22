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
#include <thread/thread.h>

struct apc {
    apc_func_t func;
    void *ctx;
    struct thread *owner;
    struct apc *next;
};

struct event_apc {
    struct apc apc;
    struct apc_event_desc *desc;
    size_t execute_times;
};

struct apc_event_desc {
    const char *name; /* TODO: add fields to this structure */
};

#define APC_EVENT_EXTERN(n) extern struct apc_event_desc __apc_event_##n

#define APC_EVENT_CREATE(n, strname)                                           \
    struct apc_event_desc __apc_event_##n = {.name = strname}
#define APC_EVENT(n) &(__apc_event_##n)

struct apc *apc_create(void);
struct event_apc *apc_event_apc_create(void);
void apc_init(struct apc *a, apc_func_t fn, void *arg1);
void apc_event_apc_init(struct event_apc *a, apc_func_t fn, void *arg1);
void apc_event_signal(struct apc_event_desc *desc);
void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type);
void apc_enqueue_event_apc(struct event_apc *a, struct apc_event_desc *d);

bool apc_cancel(struct apc *a);

void apc_check_and_deliver(struct thread *t);

void apc_enable_special();
void apc_disable_special();

void apc_enable_kernel();
void apc_disable_kernel();

void apc_free_on_thread(struct thread *t);

static inline const char *apc_event_str(struct apc_event_desc *evt) {
    return evt->name;
}

static inline void apc_enqueue_on_curr(struct apc *a, enum apc_type type) {
    apc_enqueue(thread_get_current(), a, type);
}

static inline void apc_queue_init(struct apc_queue *q) {
    q->head = q->tail = NULL;
}
