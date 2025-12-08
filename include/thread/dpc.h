/* @title: DPCs */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*dpc_func_t)(void *ctx);

/* User-facing DPC object */
struct dpc {
    dpc_func_t func;
    void *ctx;
    _Atomic(struct dpc *) next; /* for MPSC push */
    _Atomic(bool) enqueued;     /* prevents double-enqueue */
    _Atomic(bool) executed;
};

/* Per-cpu DPC data */
struct dpc_cpu {
    _Atomic(struct dpc *) head;  /* producers push to head (lock-free) */
    _Atomic(uint8_t) dpc_queued; /* 0/1 - whether an IPI has been queued */
};

void dpc_run_local(void);
struct dpc *dpc_create(dpc_func_t fn, void *ctx);
void dpc_init_percpu(void);
bool dpc_enqueue_local(struct dpc *d);
bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d);
void dpc_run_local(void);
