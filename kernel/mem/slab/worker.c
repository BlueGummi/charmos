/* Implements slab workers and daemon threads */

#include <sch/defer.h>
#include <sch/daemon.h>

#include "gc_internal.h"

/* Ok, there's quite a bit of background work to be done. */
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

