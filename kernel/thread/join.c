#include <thread/thread.h>

static void join_check(struct thread *t) {
    kassert(t);
    kassert(t != thread_get_current(), "thread '%s' joined itself", t->name);
    kassert(irql_get() == IRQL_PASSIVE_LEVEL, "thread_join() blocks");

    enum thread_flags f = thread_or_flags(t, THREAD_FLAG_JOINED);
    kassert(f & THREAD_FLAG_JOINABLE, "join on a detached thread '%s'",
            t->name);
    kassert(!(f & THREAD_FLAG_JOINED), "thread '%s' joined twice", t->name);
}

int thread_join(struct thread *t) {
    join_check(t);

    enum irql irql = spin_lock(&t->join_lock);
    while (thread_get_state(t) != THREAD_STATE_ZOMBIE)
        condvar_wait(&t->join_cv, &t->join_lock, irql, &irql);

    int status = t->exit_status;
    spin_unlock(&t->join_lock, irql);

    thread_put(t);
    return status;
}

bool thread_join_timeout(struct thread *t, time_ms_t timeout_ms,
                         int *status_out) {
    join_check(t);

    enum irql irql = spin_lock(&t->join_lock);
    while (thread_get_state(t) != THREAD_STATE_ZOMBIE) {
        enum wake_reason r = condvar_wait_timeout(&t->join_cv, &t->join_lock,
                                                  timeout_ms, irql, &irql);

        if (r == WAKE_REASON_TIMEOUT &&
            thread_get_state(t) != THREAD_STATE_ZOMBIE) {
            spin_unlock(&t->join_lock, irql);
            /* caller must retry the join or detach */
            thread_and_flags(t, ~THREAD_FLAG_JOINED);
            return false;
        }
    }

    if (status_out)
        *status_out = t->exit_status;

    spin_unlock(&t->join_lock, irql);

    thread_put(t);
    return true;
}

void thread_detach(struct thread *t) {
    enum thread_flags f = thread_and_flags(t, ~THREAD_FLAG_JOINABLE);
    kassert(f & THREAD_FLAG_JOINABLE);
    kassert(!(f & THREAD_FLAG_JOINED), "detach races an in-flight join");

    thread_put(t);
}
