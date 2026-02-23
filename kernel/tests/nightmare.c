#include <log.h>
#include <nightmare_test.h>
#include <string.h>

LOG_SITE_DECLARE_DEFAULT(nightmare);
LOG_HANDLE_DECLARE_DEFAULT(nightmare);

#define nightmare_log(lvl, fmt, ...)                                           \
    log(LOG_SITE(nightmare), LOG_HANDLE(nightmare), lvl, fmt, ##__VA_ARGS__)

#define nightmare_err(fmt, ...) nightmare_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define nightmare_warn(fmt, ...) nightmare_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define nightmare_info(fmt, ...) nightmare_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define nightmare_debug(fmt, ...) nightmare_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define nightmare_trace(fmt, ...) nightmare_log(LOG_TRACE, fmt, ##__VA_ARGS__)

void nightmare_default_report_writer(struct nightmare_report *r, const char *c,
                                     size_t s) {
    size_t where_to_start = r->buffer_len;
    r->buffer_len += s;
    r->buffer = krealloc(r->buffer, r->buffer_len, ALLOC_PARAMS_DEFAULT);
    memcpy(r->buffer + where_to_start, c, s);
}

void nightmare_tests_start(void) {
    struct nightmare_test *t;

    for (t = __skernel_nightmare_tests; t < __ekernel_nightmare_tests; t++) {

        struct nightmare_watchdog wd;
        nightmare_watchdog_init(&wd);
        t->watchdog = &wd;

        NIGHTMARE_SET_STATE(t, NIGHTMARE_UNINIT);
        if (t->reset)
            t->reset(t);

        if (t->init)
            t->init(t);

        NIGHTMARE_SET_STATE(t, NIGHTMARE_READY);

        NIGHTMARE_SET_STATE(t, NIGHTMARE_RUNNING);
        atomic_store(&t->error, NIGHTMARE_ERR_OK);

        enum nightmare_test_error err = NIGHTMARE_ERR_OK;

        if (t->start)
            err = t->start(t);

        atomic_store(&t->error, err);

        if (err == NIGHTMARE_ERR_OK || err == NIGHTMARE_ERR_FAIL) {
            if (t->stop)
                t->stop(t);
        } else {
            if (t->shutdown)
                t->shutdown(t);
        }

        NIGHTMARE_SET_STATE(t, NIGHTMARE_STOPPED);

        struct nightmare_report rpt = {
            .buffer = NULL,
            .buffer_len = 0,
            .write_fn = nightmare_default_report_writer,
            .flags = 0,
        };

        if (t->report)
            t->report(t, &rpt);

        nightmare_info("test %-20s => %d\n", t->name, err);
    }
}

static size_t nightmare_count_threads(struct nightmare_test *nt) {
    size_t acc = 0;
    for (size_t i = 0; i < nt->role_count; i++) {
        acc += nt->roles[i].count;
    }
    return acc;
}

void nightmare_spawn_roles(struct nightmare_test *test,
                           struct nightmare_thread_group *group) {
    group->threads =
        kmalloc(nightmare_count_threads(test) * sizeof(struct nightmare_thread),
                ALLOC_PARAMS_DEFAULT);

    size_t g_idx = 0;
    for (size_t i = 0; i < test->role_count; i++) {
        struct nightmare_role *nr = &test->roles[i];
        struct nightmare_thread *nt = &group->threads[g_idx];

        for (size_t j = 0; j < nr->count; j++) {
            nt->role = nr->type;
            nt->test = test;
            nt->th = thread_create((char *) nr->name, nr->worker, nr->arg);
            nt->th->private = nt;

            scheduler_enqueue(nt->th);

            g_idx++;
        }
    }
}

void nightmare_join_roles(struct nightmare_thread_group *ntg) {
    for (size_t i = 0; i < ntg->count; i++)
        if (atomic_load(&ntg->threads[i].th))
            scheduler_yield();
}
