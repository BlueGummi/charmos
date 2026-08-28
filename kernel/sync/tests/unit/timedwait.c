#include "../test_internal.h"
#include <mem/alloc_or_die.h>
#include <sync/completion.h>
#include <sync/condvar.h>
#include <sync/semaphore.h>

#ifdef TEST_QSPINLOCK

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
        atomic_fetch_add(&race->completed, 1);
    }
}

TEST_DECLARE_UNIT(qspinlock, condvar_timeout_does_not_lose_wake) {
    struct condvar_timeout_race race = {0};
    condvar_init(&race.cv, CONDVAR_INIT_NORMAL);
    spinlock_init(&race.lock);

    struct thread *t = thread_spawn_joinable(
        "condvar_timeout_race", condvar_timeout_race_worker, &race);
    TEST_ASSERT_NONNULL(t);

    bool joined = thread_join_timeout(t, 2500, NULL);
    if (!joined) {
        test_info("condvar timeout wake lost after %zu completed waits",
                  atomic_load(&race.completed));
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

TEST_DECLARE_UNIT(qspinlock, semaphore_timedwait_success_and_timeout) {
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
        alloc_or_die(thread_create("sem_poster", timed_sem_poster, &a));
    thread_enqueue(t);

    TEST_ASSERT(semaphore_timedwait(&s, 200));
    TEST_ASSERT_EQ(s.count, 0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(qspinlock, completion_timedwait_success_and_timeout) {
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
        alloc_or_die(thread_create("comp_signaler", timed_comp_signaler, &a));
    thread_enqueue(t);

    TEST_ASSERT(completion_wait_timeout(&c, 200));
    TEST_ASSERT(!completion_done(&c));

    struct thread *t2 =
        alloc_or_die(thread_create("comp_all", timed_comp_all_signaler, &a));
    thread_enqueue(t2);

    TEST_ASSERT(completion_wait_timeout(&c, 200));
    TEST_ASSERT(completion_done(&c));

    return TEST_SUCCESS;
}

#endif /* TEST_QSPINLOCK */
