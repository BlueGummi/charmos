/* @title: DPCs */
#pragma once
#include <atomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/cpu_mask.h>

struct dpc;
struct dpc_context;

typedef void (*dpc_func_t)(struct dpc_context *d, void *a, void *b);

struct dpc {
    dpc_func_t func;
    void *a;
    void *b;
    atomic(struct dpc *) next; /* for MPSC push */
    atomic_bool enqueued;      /* prevents double-enqueue */
};

struct dpc_context {
    struct dpc *dpc;
};

typedef void (*dpc_fanout_fn_t)(void);

struct dpc_queue {
    atomic(struct dpc *) head;
};

/* Per-cpu DPC data */
struct dpc_cpu {
    struct dpc_queue queue;
};

void dpc_drain_local(void);
void dpc_run_local(void);
void dpc_run_dpcs_from_irq(void);
struct dpc *dpc_create(dpc_func_t fn, void *a, void *b);
struct dpc *dpc_init(struct dpc *d, dpc_func_t fn, void *a, void *b);

/* Run fn once on every CPU in cpus, returning only after all of them complete.
 *
 * NOTE: storage must hold at least popcount(cpus) DPCs, this is what
 * the caller must uphold. storage entries get overwritten every call as well */
void dpc_fanout(struct dpc *storage, struct cpu_mask cpus, dpc_fanout_fn_t fn);
void dpc_init_percpu(void);
bool dpc_enqueue_local(struct dpc *d);
bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d);
