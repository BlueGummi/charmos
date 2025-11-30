/* Implements slab workers and daemon threads */

#include <sch/daemon.h>
#include <sch/defer.h>
#include <smp/domain.h>

#include "gc_internal.h"

/* Ok, there's quite a bit of background work to be done. */

/* We first want to go ahead and flush our freequeue. We'll trylock
 * the individual per-cpu caches to see if we can sneak some of the
 * elements in there */
enum daemon_thread_command slab_background_work(struct daemon_work *work,
                                                struct daemon_thread *thread,
                                                void *a, void *b) {
    return DAEMON_THREAD_COMMAND_DEFAULT;
}

static struct daemon_work bg =
    DAEMON_WORK_FROM(slab_background_work, WORK_ARGS(NULL, NULL));

void slab_domain_init_daemon(struct slab_domain *domain) {
    struct daemon_attributes attrs = {
        .max_timesharing_threads = 0,
        .thread_cpu = domain->domain->cores[0]->id,
        .flags = DAEMON_FLAG_UNMIGRATABLE_THREADS | DAEMON_FLAG_NO_TS_THREADS |
                 DAEMON_FLAG_HAS_NAME,
    };

    domain->daemon = daemon_create(
        /* attrs = */ &attrs,
        /* timesharing_work = */ NULL,
        /* background_work = */ &bg,
        /* wq_attrs = */ NULL,
        /* fmt = */ "slab_domain_%u",
        /* ... = */ domain->domain->id);
}

void slab_domain_init_workqueue(struct slab_domain *domain) {
    struct cpu_mask mask = {0};
    if (!cpu_mask_init(&mask, global.core_count))
        k_panic("CPU mask initialization failed\n");

    domain_set_cpu_mask(&mask, domain->domain);

    struct workqueue_attributes attrs = {
        .capacity = WORKQUEUE_DEFAULT_CAPACITY,

        /* We set static_workers and spawn_via_request to make these safe
         * here since if those weren't set the dynamic memory allocation
         * could potentially spiral into bigger problems... */
        .flags = WORKQUEUE_FLAG_DEFAULTS | WORKQUEUE_FLAG_STATIC_WORKERS |
                 WORKQUEUE_FLAG_SPAWN_VIA_REQUEST | WORKQUEUE_FLAG_NAMED |
                 WORKQUEUE_FLAG_NO_WORKER_GC,
        .max_workers = domain->domain->num_cores,
        .min_workers = 1,
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
        .worker_cpu_mask = mask,
    };

    domain->workqueue =
        workqueue_create(&attrs, "slab_domain%u_wq", domain->domain->id);
}
