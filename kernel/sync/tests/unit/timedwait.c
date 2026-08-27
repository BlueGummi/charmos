#include "../test_internal.h"
#include <mem/alloc_or_die.h>
#include <sync/completion.h>
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

TEST_DECLARE_UNIT(semaphore_timedwait_success_and_timeout,
                  .group = TEST_GROUP(qspinlock)) {
    struct semaphore s;
    semaphore_init(&s, 1, false);

    TEST_ASSERT(semaphore_timedwait(&s, 50));
    TEST_ASSERT(s.count == 0);

    time_ms_t t0 = time_get_ms();
    TEST_ASSERT(!semaphore_timedwait(&s, 30));
    time_ms_t elapsed = time_get_ms() - t0;
    TEST_ASSERT(elapsed >= 25);
    TEST_ASSERT(s.count == 0);

    struct timed_helper_args a = {
        .sem = &s,
        .delay_ms = 20,
    };
    struct thread *t =
        alloc_or_die(thread_create("sem_poster", timed_sem_poster, &a));
    thread_enqueue(t);

    TEST_ASSERT(semaphore_timedwait(&s, 200));
    TEST_ASSERT(s.count == 0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(completion_timedwait_success_and_timeout,
                  .group = TEST_GROUP(qspinlock)) {
    struct completion c;
    completion_init(&c, false);

    time_ms_t t0 = time_get_ms();
    TEST_ASSERT(!completion_wait_timeout(&c, 30));
    time_ms_t elapsed = time_get_ms() - t0;
    TEST_ASSERT(elapsed >= 25);

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
