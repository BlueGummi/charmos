#include <global.h>
#include <log.h>
#include <math/fixed.h>
#include <mem/alloc.h>
#include <mem/nofail.h>
#include <sch/rt_sched.h>
#include <sch/sched.h>
#include <smp/core.h>

#include "internal.h"

LOG_SITE_DEFINE(rt_sched, .flags = LOG_SITE_DEFAULT,
                .capacity = LOG_SITE_CAPACITY_DEFAULT,
                .enabled_mask = LOG_SITE_ALL,
                .dump_opts = (struct log_dump_options){});

static void init_scheduler_boot(struct scheduler *sched) {
    struct rt_scheduler_percpu *pcpu = nofail_vnew(pcpu, ALLOC_ZERO);

    struct log_site_options opts = {
        .name = "rt_sched",
        .capacity = LOG_SITE_CAPACITY_DEFAULT,
        .enabled_mask = LOG_SITE_ALL,
        .dump_opts = (struct log_dump_options){},
        .flags = LOG_SITE_DEFAULT,
    };

    pcpu->log_site = nofail(log_site_create(opts));

    pcpu->log_handle = LOG_HANDLE_DEFAULT;
    pcpu->perms.allowed_capabilities = UINT16_MAX;
    spinlock_init(&pcpu->perms.lock);
    INIT_LIST_HEAD(&pcpu->perms.blocklist);
    pcpu->scheduler = sched;
    pcpu->active_mapping = NULL;
    semaphore_init(&pcpu->switch_semaphore, 1, SEMAPHORE_INIT_IRQ_DISABLE);

    struct rt_scheduler *rts = nofail_vnew(rts, ALLOC_ZERO);

    rts->log_site = nofail(log_site_create(opts));

    rts->log_handle = LOG_HANDLE_DEFAULT;
    spinlock_init(&rts->lock);
    pcpu->born_with = rts;
    sched->rt = pcpu;
    rts->failed_internal = false;
    rts->mapping_source = NULL;
    rts->mapping_source = NULL;
}

void rt_scheduler_boot_init() {
    rt_wq = nofail(workqueue_create_default("rt_wq"));

    locked_list_init(&rt_global.static_list, LOCKED_LIST_INIT_IRQ_DISABLE);
    spinlock_init(&rt_global.switch_lock);
    rt_global.sch_pool =
        nofail_vnews(rt_global.sch_pool, global.domain_count, ALLOC_ZERO);

    struct domain *d;
    domain_for_each_domain(d) {
        locked_list_init(&rt_global.sch_pool[d->id],
                         LOCKED_LIST_INIT_IRQ_DISABLE);
    }

    struct core *c;
    for_each_cpu_struct(c) {
        struct scheduler *s = global.schedulers[c->id];
        init_scheduler_boot(s);
    }
}
