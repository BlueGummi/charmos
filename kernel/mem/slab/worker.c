/* Implements slab workers and daemon threads */
#include <mem/alloc_or_die.h>
#include <smp/domain.h>
#include <thread/daemon.h>
#include <thread/workqueue.h>

#include "gc_internal.h"

/* Ok, there's quite a bit of background work to be done. */

/* We first want to go ahead and flush our freequeue. We'll trylock
 * the individual per-cpu caches to see if we can sneak some of the
 * elements in there */
static enum daemon_thread_command slab_background_work(void *a, void *b) {
    /* TODO: */
    return DAEMON_THREAD_COMMAND_DEFAULT;
}

/* Aside on deferred frees:
 *
 * We can guarantee that any slab object is at minimum pointer sized
 *
 * This means we can embed a *next pointer in every object to another
 * object. The idea is this:
 *
 * The address of every node is what we free. We use the *next at the
 * node to get to the next one to free it too. SLIST semantics
 * mean that this can be MPSC, which is what we're looking to be
 * able to pull off here.
 */
static void slab_defer_free_dpc(void *unused_arg) {
    (void) unused_arg;
    struct slab_percpu_cache *c = slab_percpu_cache_local();
    /* Enter a loop here of stealing the defer free list,
     * and so long as we actually steal something, we
     * drain whatever is on that list */

    struct mpsc_slist_node *tmp, *iter, *got = NULL;
    while ((got = mpsc_slist_drain(&c->defer_frees))) {
        got = mpsc_slist_reverse(got);
        mpsc_slist_for_each_safe(iter, tmp, got) {
            kfree(iter);
        }
    }
}

static struct daemon_work bg =
    DAEMON_WORK_FROM(slab_background_work, WORK_ARGS(NULL, NULL));

void slab_domain_init_daemon(struct slab_domain *domain) {
    struct cpu_mask cmask;
    alloc_or_die(cpu_mask_init(&cmask, global.core_count));

    domain_set_cpu_mask(&cmask, domain->domain);
    struct daemon_attributes attrs = {
        .max_timesharing_threads = 0,
        .min_timesharing_threads = 0,
        .thread_cpu_mask = cmask,
        .flags = DAEMON_FLAG_NO_TS_THREADS | DAEMON_FLAG_HAS_NAME,
    };

    domain->daemon = daemon_create(
        /* fmt = */ "slab_domain_%u",
        /* attrs = */ &attrs,
        /* timesharing_work = */ NULL,
        /* background_work = */ &bg,
        /* wq_attrs = */ NULL,
        /* ... = */ domain->domain->id);
}

void slab_domain_init_workqueue(struct slab_domain *domain) {
    struct cpu_mask mask = {0};
    alloc_or_die(cpu_mask_init(&mask, global.core_count));

    domain_set_cpu_mask(&mask, domain->domain);

    struct workqueue_attributes attrs = {
        .capacity = WORKQUEUE_DEFAULT_CAPACITY,

        /* We set static_workers and spawn_via_request to make these safe
         * here since if those weren't set the dynamic memory allocation
         * could potentially spiral into bigger problems... */
        .flags = WORKQUEUE_FLAG_STATIC_WORKERS | WORKQUEUE_FLAG_NAMED |
                 WORKQUEUE_FLAG_NO_WORKER_GC,
        .max_workers = domain->domain->num_cores,
        .min_workers = 1,
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
        .worker_cpu_mask = mask,
        .worker_niceness = 0,
        .idle_check = WORKQUEUE_DEFAULT_IDLE_CHECK,
    };

    domain->workqueue =
        workqueue_create("slab_domain_%u_wq", &attrs, domain->domain->id);
    for (size_t i = 0; i < domain->domain->num_cores; i++) {
        struct dpc *defer_dpc = &domain->percpu_caches[i]->defer_dpc;
        dpc_init(defer_dpc, slab_defer_free_dpc, NULL);
    }
}

void kfree_defer_irq(void *ptr) {
    kassert(irq_in_interrupt());
    if (!ptr)
        return;

    struct slab_percpu_cache *pc = slab_percpu_cache_local();
    struct mpsc_slist_node *n = ptr;
    mpsc_slist_push(&pc->defer_frees, n);
    dpc_enqueue_local(&pc->defer_dpc, DPC_NONE);
}
