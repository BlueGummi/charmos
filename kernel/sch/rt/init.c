#include <global.h>
#include <log.h>
#include <math/fixed.h>
#include <mem/alloc.h>
#include <sch/rt_sched.h>
#include <sch/sched.h>
#include <smp/core.h>

LOG_SITE_DECLARE(rt_sched, .flags = LOG_SITE_DEFAULT,
                 .capacity = LOG_SITE_CAPACITY_DEFAULT,
                 .enabled_mask = LOG_SITE_ALL,
                 .dump_opts = (struct log_dump_options){});

static size_t rt_mapping_get_data(struct rbt_node *n) {
    return container_of(n, struct rt_scheduler_mapping, tree_node)->id;
}

static int32_t rt_mapping_cmp(const struct rbt_node *a,
                              const struct rbt_node *b) {
    int32_t l = rt_mapping_get_data((void *) a);
    int32_t r = rt_mapping_get_data((void *) b);
    return l - r;
}

static void reset_summary(struct rt_thread_summary *sum) {
    sum->status = RT_SCHEDULER_STATUS_OK;
    sum->total_weight = 0;
    sum->urgency = FX(0.0);
}

static void reset_shed_request(struct rt_thread_shed_request *rtsr) {
    rtsr->urgency = FX(0.0);
    rtsr->threads_available = 0;
    kassert(list_empty(&rtsr->threads));
    INIT_LIST_HEAD(&rtsr->threads);
    rtsr->on = false;
}

static void reset_scheduler(struct rt_scheduler *rts) {
    rts->failed_internal = false;
    atomic_store_explicit(&rts->state, RT_SCHEDULER_UNINIT,
                          memory_order_release);
    rts->mapping_source = NULL;
    kassert(list_empty(&rts->thread_list));
    INIT_LIST_HEAD(&rts->thread_list);
    rts->mapping_source = NULL;
}

static void init_scheduler_boot(struct scheduler *sched) {
    struct rt_scheduler_percpu *pcpu =
        kzalloc(sizeof(struct rt_scheduler_percpu), ALLOC_PARAMS_DEFAULT);
    if (!pcpu)
        panic("OOM\n");

    pcpu->scheduler = sched;
    pcpu->active = NULL;

    struct rt_scheduler *rts =
        kzalloc(sizeof(struct rt_scheduler), ALLOC_PARAMS_DEFAULT);
    if (!rts)
        panic("OOM\n");

    spinlock_init(&rts->lock);
    pcpu->born_with = rts;
    sched->rt = pcpu;
    rts->core_scheduler = sched;
    INIT_LIST_HEAD(&rts->thread_list);
    reset_scheduler(rts);
}

void rt_scheduler_boot_init() {
    locked_list_init(&rt_scheduler_global.list);
    struct core *c;
    for_each_cpu_struct(c) {
        struct scheduler *s = global.schedulers[c->id];
        init_scheduler_boot(s);
    }
}
