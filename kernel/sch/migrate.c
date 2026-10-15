#include "internal.h"
#include <thread/apc.h>

/* file implements `thread_migrate`.
 *
 * NOTE: when we acquire two `struct scheduler` locks,
 * we ALWAYS ALWAYS acquire the lock for the LOWER
 * memory address first, same for unlock,
 * and we ALWAYS acquire thread
 * locks AFTER any scheduler locks! */

void thread_post_migrate(struct thread *t, size_t old_cpu, size_t new_cpu) {
    /* assert that both scheduler/runqueue locks are held,
     * migrate relevant things */
    SPINLOCK_ASSERT_HELD(&global.schedulers[old_cpu]->lock);
    SPINLOCK_ASSERT_HELD(&global.schedulers[new_cpu]->lock);
    climb_post_migrate_hook(t, old_cpu, new_cpu);
}

void thread_migrate(struct thread *t, size_t dest_core) TSA_NO_ANALYSIS {
    /* first acquire both the lock of the thread's scheduler
     * and the destination core's scheduler */

    enum irql sirql, dirql, tirql;

    struct scheduler *src;
    struct scheduler *dst = global.schedulers[dest_core];

    thread_lock_thread_and_rq(t, dst, &src, &sirql, &dirql);

    if (src == dst) {
        thread_unlock_thread_and_rq(src, dst, sirql, dirql);
        return;
    }

    bool ok;
    tirql = thread_acquire(t, &ok);
    if (!ok) {
        goto end;
    }

    /* bro cannot be migrated */
    if (!scheduler_can_take_thread(dest_core, t)) {
        goto out;
    }

    /* finally, we can do something here. */
    if (thread_get_state(t) == THREAD_STATE_RUNNING ||
        !(thread_get_flags(t) & THREAD_FLAG_YIELDED)) {
        thread_set_migration_target(t, dest_core);
        scheduler_force_resched(dst);
    } else if (thread_get_state(t) == THREAD_STATE_READY) {
        scheduler_remove_thread_locked(src, t);
        scheduler_add_thread_locked(dst, t);
        scheduler_force_resched(dst);
    }

    thread_set_runqueue(t, dst);

out:

    thread_release(t, tirql);

end:
    thread_unlock_thread_and_rq(src, dst, sirql, dirql);
}
