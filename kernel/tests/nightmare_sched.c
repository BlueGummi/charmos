#include <nightmare_test.h>

/* ---------- Worker Thread ---------- */

NIGHTMARE_THREAD_ENTRY(thread_spawn_smoke_worker) {
    NIGHTMARE_THREAD_ENTRY_INIT();
    struct nightmare_thread *nth = nightmare_get_thread();
    atomic_uint *counter = (atomic_uint *) nth->local.data;

    /* Do a little chaos to exercise scheduler */
    nightmare_chaos_pause();

    /* Mark that this thread ran */
    atomic_fetch_add(counter, 1);

    NIGHTMARE_PROGRESS();
    NIGHTMARE_THREAD_ENTRY_EXIT();
}

/* ---------- Reset / Init ---------- */

NIGHTMARE_IMPL_RESET(thread_spawn_smoke) {
    NIGHTMARE_FN_INIT();
    /* Nothing to clean up (for now) */
}

NIGHTMARE_IMPL_INIT(thread_spawn_smoke) {
    NIGHTMARE_FN_INIT();
    /* nothing special to set up */
}

/* ---------- Start / Stop ---------- */

NIGHTMARE_IMPL_START(thread_spawn_smoke) {
    const size_t nthreads = SELF->default_threads;

    atomic_uint *counter = kmalloc(sizeof(*counter), ALLOC_PARAMS_DEFAULT);
    *counter = 0;

    /* share through thread-local */
    nightmare_set_local(counter, sizeof(*counter));

    /* Add a single role: N worker threads */
    nightmare_add_role(SELF, NIGHTMARE_ROLE_GENERIC, "spawn_smoke_worker",
                       thread_spawn_smoke_worker, nthreads, counter);

    /* Spawn threads */
    struct nightmare_thread_group grp;
    nightmare_spawn_roles(SELF, &grp);

    /* Wait until every worker has incremented once */
    time_t timeout = SELF->default_runtime_ms;

    while (atomic_load(counter) < nthreads) {
        NIGHTMARE_PROGRESS();

        if (nightmare_watchdog_expired(SELF->watchdog, timeout))
            NIGHTMARE_RETURN_ERROR(NIGHTMARE_ERR_FAIL);

        scheduler_yield();
    }

    /* Join worker threads */
    nightmare_join_roles(&grp);

    NIGHTMARE_RETURN_ERROR(NIGHTMARE_ERR_OK);
}

NIGHTMARE_IMPL_STOP(thread_spawn_smoke) {
    NIGHTMARE_FN_INIT();
    /* Graceful stop not needed, workers already exit */
}

NIGHTMARE_IMPL_SHUTDOWN(thread_spawn_smoke) {
    NIGHTMARE_FN_INIT();
    /* Forced stop not needed */
}

/* ---------- Report ---------- */

NIGHTMARE_IMPL_REPORT(thread_spawn_smoke) {
    NIGHTMARE_FN_INIT();
    REPORT->write_fn(REPORT, "thread_spawn_smoke: all threads ran\n", 38);
}

/* ---------- Register Test ---------- */

NIGHTMARE_DEFINE_TEST(thread_spawn_smoke, 2000, /* 2 seconds */
                      5000);                    /* spawn 5000 threads */
