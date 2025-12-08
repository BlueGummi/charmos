/* @title: DPCs */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum dpc_event {
    DPC_NONE,     /* No event */
    DPC_CPU_IDLE, /* CPU went idle */
    DPC_CPU_WOKE, /* CPU went un-idle */
    DPC_EVENT_MAX,
};

struct dpc;
typedef void (*dpc_func_t)(struct dpc *, void *ctx);

struct dpc {
    dpc_func_t func;
    void *ctx;
    _Atomic(struct dpc *) next; /* for MPSC push */
    _Atomic(bool) enqueued;     /* prevents double-enqueue */
    _Atomic(bool) executed;
};

struct dpc_queue {
    _Atomic(struct dpc *) head;
    _Atomic size_t count;
};

/* Per-cpu DPC data */
struct dpc_cpu {
    _Atomic(bool) ipi_queued;
    struct dpc_queue queues[DPC_EVENT_MAX];
};

void dpc_run_local(void);
struct dpc *dpc_create(dpc_func_t fn, void *ctx);
struct dpc *dpc_init(struct dpc *d, dpc_func_t fn, void *ctx);
void dpc_init_percpu(void);
bool dpc_enqueue_local(struct dpc *d, enum dpc_event e);
bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d, enum dpc_event e);
