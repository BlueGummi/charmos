#include "sync/tests/test_internal.h"
#include <mem/nofail.h>
#include <sync/completion.h>
#include <sync/condvar.h>
#include <sync/semaphore.h>

TEST_GROUP_DEFINE(condvar);
TEST_GROUP_DEFINE(semaphore);
TEST_GROUP_DEFINE(completion);

struct timed_helper_args {
    struct semaphore *sem;
    struct completion *comp;
    time_ms_t delay_ms;
};

static void timed_sem_poster(void *arg) {
    struct timed_helper_args *a = arg;
    sleep_spin_ms(a->delay_ms);
    semaphore_post(a->sem);
}

static void timed_comp_signaler(void *arg) {
    struct timed_helper_args *a = arg;
    sleep_spin_ms(a->delay_ms);
    complete(a->comp);
}

static void timed_comp_all_signaler(void *arg) {
    struct timed_helper_args *a = arg;
    sleep_spin_ms(a->delay_ms);
    complete_all(a->comp);
}

struct condvar_timeout_race {
    struct condvar cv;
    struct spinlock lock;
    atomic_bool stop;
    atomic_size_t completed;
    atomic_bool wrong_reason;
};

static void condvar_timeout_race_worker(void *arg) {
    struct condvar_timeout_race *race = arg;

    for (size_t i = 0; i < 3000 && !atomic_load(&race->stop); i++) {
        enum irql irql = spin_lock(&race->lock);
        enum wake_reason reason =
            condvar_wait_timeout(&race->cv, &race->lock, 0, irql, &irql);
        spin_unlock(&race->lock, irql);

        if (reason != WAKE_REASON_TIMEOUT)
            atomic_store(&race->wrong_reason, true);
        atomic_inc(&race->completed);
    }
}

TEST_DEFINE_UNIT(condvar, timeout_no_lost_wake) {
    static const time_ms_t progress_timeout_ms = 1000;
    static const size_t stalled_progress_limit = 2;

    struct condvar_timeout_race race = {0};
    condvar_init(&race.cv);
    spinlock_init(&race.lock);

    struct thread *t =
        thread_spawn("condvar_timeout_race", condvar_timeout_race_worker,
                     .arg = &race, .joinable = true);
    TEST_ASSERT_NONNULL(t);

    bool joined = false;
    size_t last_completed = 0;
    size_t stalled_progress = 0;

    while (!(joined = thread_join_timeout(t, progress_timeout_ms, NULL))) {
        size_t completed = atomic_load(&race.completed);
        if (completed == last_completed) {
            stalled_progress++;
        } else {
            last_completed = completed;
            stalled_progress = 0;
        }

        if (stalled_progress == stalled_progress_limit)
            break;
    }

    if (!joined) {
        test_info("condvar timeout worker stalled after %zu completed waits",
                  last_completed);
        atomic_store(&race.stop, true);
        enum irql irql = spin_lock(&race.lock);
        condvar_signal(&race.cv);
        spin_unlock(&race.lock, irql);
        thread_join(t);
    }

    TEST_ASSERT(joined);
    TEST_ASSERT_EQ(3000, atomic_load(&race.completed));
    TEST_ASSERT(!atomic_load(&race.wrong_reason));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(semaphore, timedwait_standard) {
    struct semaphore s;
    semaphore_init(&s, 1, false);

    TEST_ASSERT(semaphore_timedwait(&s, 50));
    TEST_ASSERT_EQ(s.count, 0);

    time_ms_t t0 = time_get_ms();
    TEST_ASSERT(!semaphore_timedwait(&s, 30));
    time_ms_t elapsed = time_get_ms() - t0;
    TEST_ASSERT_GE(elapsed, 25);
    TEST_ASSERT_EQ(s.count, 0);

    struct timed_helper_args a = {
        .sem = &s,
        .delay_ms = 20,
    };
    struct thread *t =
        nofail(thread_create("sem_poster", timed_sem_poster, .arg = &a));
    thread_enqueue(t);

    TEST_ASSERT(semaphore_timedwait(&s, 200));
    TEST_ASSERT_EQ(s.count, 0);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(completion, wait_timeout_standard) {
    struct completion c;
    completion_init(&c, false);

    time_ms_t t0 = time_get_ms();
    TEST_ASSERT(!completion_wait_timeout(&c, 30));
    time_ms_t elapsed = time_get_ms() - t0;
    TEST_ASSERT_GE(elapsed, 25);

    struct timed_helper_args a = {
        .comp = &c,
        .delay_ms = 20,
    };
    struct thread *t =
        nofail(thread_create("comp_signaler", timed_comp_signaler, .arg = &a));
    thread_enqueue(t);

    TEST_ASSERT(completion_wait_timeout(&c, 200));
    TEST_ASSERT(!completion_done(&c));

    struct thread *t2 =
        nofail(thread_create("comp_all", timed_comp_all_signaler, .arg = &a));
    thread_enqueue(t2);

    TEST_ASSERT(completion_wait_timeout(&c, 200));
    TEST_ASSERT(completion_done(&c));

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(completion, static_init) {
    struct completion c = COMPLETION_INIT(c, COMPLETION_INIT_NORMAL);
    TEST_ASSERT(list_head_empty(&c.cv.waiters.waiters));
    TEST_ASSERT(!completion_try_wait(&c));
    complete(&c);
    TEST_ASSERT(completion_try_wait(&c));
    TEST_ASSERT(!completion_try_wait(&c));
    return TEST_SUCCESS;
}
