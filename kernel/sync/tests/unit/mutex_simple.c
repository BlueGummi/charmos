#include "sync/tests/test_internal.h"
#include <sync/mutex_simple.h>

TEST_GROUP_DEFINE(mutex_simple);

struct simple_contention {
    struct mutex_simple mutex;
    size_t count;
};

static void simple_contender(void *arg) {
    struct simple_contention *c = arg;
    for (size_t i = 0; i < 32; i++) {
        mutex_simple_lock(&c->mutex);
        c->count++;
        scheduler_yield(); /* force other thread through wait path */
        mutex_simple_unlock(&c->mutex);
    }
}

TEST_DEFINE_UNIT(mutex_simple, object_wait_handoff) {
    struct simple_contention c = {0};
    mutex_simple_init(&c.mutex);
    struct thread *a =
        thread_spawn("simple_a", simple_contender, .arg = &c, .joinable = true);
    struct thread *b =
        thread_spawn("simple_b", simple_contender, .arg = &c, .joinable = true);
    TEST_ASSERT_NONNULL(a);
    TEST_ASSERT_NONNULL(b);
    thread_join(a);
    thread_join(b);
    TEST_ASSERT_EQ(c.count, 64);
    TEST_ASSERT(list_head_empty(&c.mutex.waiters.waiters));
    return TEST_SUCCESS;
}
