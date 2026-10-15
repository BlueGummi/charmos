#include "internal.h"

#include <nightmare/record.h>
#include <sch/sched.h>
#include <thread/thread.h>

#ifdef TEST_NIGHTMARE_ENABLED
void nightmare_publish_stop(enum test_stop reason) {
    test_conc_publish_stop(&nightmare_runtime.conc, reason);
}

bool nightmare_must_stop(void) {
    return test_conc_must_stop(&nightmare_runtime.conc);
}

void nightmare_stop_after_finding(void) {
    nightmare_publish_stop(TEST_STOP_FINDING);
}

bool nightmare_must_park(void) {
    return test_conc_must_park(&nightmare_runtime.conc);
}

void nightmare_park(struct test_conc_worker *worker) {
    test_conc_park(&nightmare_runtime.conc, worker);
}

static void nightmare_perturb_finding(const char *kind, const char *msg) {
    nightmare_finding_at(
        &(const struct nightmare_finding_site){.kind = kind,
                                               .tier = NIGHTMARE_TIER_AMBIGUOUS,
                                               .file = __RELFILE__,
                                               .line = __LINE__},
        0, "%s", msg);
}

void nightmare_thread_main(void *arg) {
    struct test_conc_worker *worker = arg;
    completion_wait(&nightmare_runtime.conc.start);

    if (!nightmare_must_stop()) {
        if (worker->index < nightmare_runtime.ctx.worker_count) {
            if (nightmare_runtime.ctx.nm && nightmare_runtime.ctx.nm->ops &&
                nightmare_runtime.ctx.nm->ops->worker)
                nightmare_runtime.ctx.nm->ops->worker(&nightmare_runtime.ctx,
                                                      worker);
        } else {
            size_t pidx = worker->index - nightmare_runtime.ctx.worker_count;
            if (pidx < nightmare_runtime.perturber_count &&
                nightmare_runtime.perturbers[pidx] &&
                nightmare_runtime.perturbers[pidx]->fn) {
                const struct test_perturb_env env = {
                    .conc = &nightmare_runtime.conc,
                    .subject_count = nightmare_runtime.ctx.worker_count,
                    .victim_cpu = TEST_PERTURB_VICTIM_ANY,
                    .finding = nightmare_perturb_finding,
                };
                nightmare_runtime.perturbers[pidx]->fn(&env, worker);
            }
        }
    }

    test_conc_unpark_self(&nightmare_runtime.conc, worker);
}

void nightmare_heartbeat_main(void *arg) {
    cc_ignore(arg);
    completion_wait(&nightmare_runtime.conc.start);

    time_ms_t next = time_get_ms() + nightmare_runtime.stat_interval_ms;
    do {
        nightmare_liveness_poll();

        time_ms_t now = time_get_ms();
        if (now >= next) {
            nightmare_record_stat(test_conc_progress_sum(),
                                  nightmare_runtime.ctx.worker_count);
            next = now + nightmare_runtime.stat_interval_ms;
        }
        thread_sleep_for_ms(10);
    } while (!nightmare_must_stop());

    nightmare_liveness_poll();

    nightmare_record_stat(test_conc_progress_sum(),
                          nightmare_runtime.ctx.worker_count);
}
#endif
