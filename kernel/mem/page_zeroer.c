#include <mem/nofail.h>
#include <mem/page_zeroer.h>
#include <smp/domain.h>
#include <smp/perdomain.h>

static enum daemon_thread_command page_zeroer_ts_thread(void *a, void *b) {
    cc_ignore(a, b);
    return DAEMON_THREAD_COMMAND_DEFAULT;
}

static enum daemon_thread_command page_zeroer_bg_thread(void *a, void *b) {
    cc_ignore(a, b);
    return DAEMON_THREAD_COMMAND_DEFAULT;
}

static struct daemon_work pz_ts_work =
    DAEMON_WORK_FROM(page_zeroer_ts_thread, WORK_ARGS(NULL, NULL));

static struct daemon_work pz_bg_work =
    DAEMON_WORK_FROM(page_zeroer_bg_thread, WORK_ARGS(NULL, NULL));

static void page_zeroer_perdomain_init(struct page_zeroer *pz,
                                       domain_id_t domain) {
    struct cpu_mask cmask;
    cpu_mask_init(&cmask, global.core_count);
    size_t threads = global.domains[domain]->num_cores / 4;
    if (!threads)
        threads = 1;

    domain_set_cpu_mask(&cmask, global.domains[domain]);
    struct daemon_attributes attrs = {
        .thread_cpu_mask = cmask,
        .max_timesharing_threads = threads,
        .min_timesharing_threads = threads,
        .flags = DAEMON_FLAG_HAS_WORKQUEUE | DAEMON_FLAG_HAS_NAME,
    };

    struct workqueue_attributes wqattrs = {
        .max_workers = threads,
        .min_workers = threads,
        .spawn_delay = 0,
        .idle_check = WORKQUEUE_DEFAULT_IDLE_CHECK,
        .worker_cpu_mask = cmask,
        .worker_niceness = 0,
        .flags = WORKQUEUE_FLAG_STATIC_WORKERS | WORKQUEUE_FLAG_ON_DEMAND |
                 WORKQUEUE_FLAG_NAMED | WORKQUEUE_FLAG_NO_WORKER_GC,
    };

    nofail(pz->daemon =
               daemon_create(("page_zeroer_daemon_%zu", domain), &attrs,
                             &pz_ts_work, &pz_bg_work, &wqattrs));
}

PERDOMAIN_DEFINE(struct page_zeroer, page_zeroers, page_zeroer_perdomain_init);
