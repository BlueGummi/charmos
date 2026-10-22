#include <sync/rwlock.h>
#include <test/fleet.h>
#include <test/sync.h>

#include "sch/tests/test_internal.h"

TEST_GROUP_DEFINE(wroost);

struct wroost_fixture {
    struct rwlock lock;
    struct test_latch reader_started;
};

static bool basic_reader(struct test_fleet *f, struct test_conc_worker *w) {
    struct wroost_fixture *fix = w->arg;

    rw_read_lock(&fix->lock);
    test_latch_set(&fix->reader_started);

    struct thread *writer = test_fleet_worker_thread(f, "writer");
    TEST_WORKER_POLL_GOTO(
        f, writer && writer->wroost_data.writer_state == WROOST_WRITER_BUMP,
        2000, out);

    scheduler_yield();

    enum thread_prio_class prio = thread_get_current()->perceived_prio_class;
    TEST_WORKER_CHECK_EQ_GOTO(f, prio, THREAD_PRIO_CLASS_REALTIME, out);

    rw_unlock(&fix->lock);
    return true;

out:
    rw_unlock(&fix->lock);
    return false;
}

static bool basic_writer(struct test_fleet *f, struct test_conc_worker *w) {
    struct wroost_fixture *fix = w->arg;

    TEST_WORKER_CHECK(f, test_latch_wait_timeout(&fix->reader_started, 2000));

    rw_write_lock(&fix->lock);
    rw_unlock(&fix->lock);
    return true;
}

TEST_DEFINE_UNIT(wroost, basic_boost, .min_cores = 2) {
    struct test_fleet *fleet = test_fleet_init(ctx, NULL);
    TEST_ASSERT_NONNULL(fleet);

    struct wroost_fixture *fix = test_alloc(ctx, sizeof(*fix));
    TEST_ASSERT_NONNULL(fix);

    rwlock_init(&fix->lock, THREAD_PRIO_CLASS_REALTIME);
    test_latch_init(&fix->reader_started);

    TEST_ASSERT_NONNULL(test_fleet_spawn_opt(
        fleet, "reader", basic_reader, .core_id = test_core(0), .arg = fix));
    TEST_ASSERT_NONNULL(test_fleet_spawn_opt(
        fleet, "writer", basic_writer, .core_id = test_core(1),
        .prio_class = THREAD_PRIO_CLASS_REALTIME, .arg = fix));

    test_fleet_start_all(fleet);
    return test_fleet_join(fleet);
}
