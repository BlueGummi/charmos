#include "internal.h"

/* file implements `thread_migrate`.
 *
 * NOTE: when we acquire two `struct scheduler` locks,
 * we ALWAYS ALWAYS acquire the lock for the LOWER
 * memory address first, same for unlock,
 * and we ALWAYS acquire thread
 * locks AFTER any scheduler locks! */

/*
 * # Small Idea: Thread being_moved locking
 *
 * ## Credits: gummi
 *
 * ## Context: This flag is used to ensure that everyone reads an accurate
 *             `last_ran` field on a thread. Basically, READY threads can be
 *             arbitrarily migrated at any time if it is possible. All
 *             migrations must occur whilst both scheduler locks are held.
 *
 *             Threads also have to have their core's scheduler lock
 *             acquired when the being_moved is being locked.
 *
 * ## Problem: We need to guarantee that this lock is used appropriately
 *             alongside thread flags to make sure that an invalid
 *             last_ran field is NEVER read. This must be done
 *             because if an invalid last_ran field is read, we will
 *             lock the WRONG scheduler and can cause catastrophic races.
 *
 * ## Strategy: Upon any thread migration within the scheduler, we spin
 *              on the lock and try to set it. Upon any read of the
 *              thread's last_ran field, we spin on the lock and wait
 *              for it to become free but never set it. Prior to any
 *              spin_raw on the lock from outside the scheduler,
 *              we ALWAYS set the thread as NO_STEAL so that it cannot
 *              possibly be migrated after the lock is available.
 *
 *              Every time we read the flags to see if the thread can
 *              be migrated, we must hold the lock prior to reading the
 *              flags, and release the lock if we are able to migrate
 *              the thread and only after we set last_ran.
 *
 * ## Changelog:
 *    12/4/2025 - gummi - Created Idea
 *
 *
 */

void thread_migrate(struct thread *t, size_t dest_core) {
    /* first acquire both the lock of the thread's scheduler
     * and the destination core's scheduler */

    enum thread_flags flags;
    struct scheduler *src = global.schedulers[thread_get_last_ran(t, &flags)];
    struct scheduler *dst = global.schedulers[dest_core];

    enum irql sirql, dirql, tirql;

    if (src == dst)
        return;

    scheduler_acquire_two_locks(src, dst, &sirql, &dirql);

    /* now that we have acquired both scheduler locks,
     * we have control over the thread and can
     * unmark it as NO_STEAL */
    thread_set_flags(t, flags);

    bool ok;
    tirql = thread_acquire(t, &ok);
    if (!ok) {
        goto end;
    }

    spin_lock_raw(&t->being_moved);
    bool unlock = true;

    /* bro cannot be migrated */
    if (!scheduler_can_steal_thread(dest_core, t)) {
        goto out;
    }

    /* finally, we can do something here. */
    if (thread_get_state(t) == THREAD_STATE_RUNNING) {
        unlock = false;
        thread_set_migration_target(t, dest_core);
        scheduler_force_resched(dst);
    } else if (thread_get_state(t) == THREAD_STATE_READY) {
        scheduler_remove_thread(src, t, /* lock_held = */ true);
        scheduler_add_thread(dst, t, /* lock_held = */ true);
    }

out:
    if (unlock)
        spin_unlock_raw(&t->being_moved);

    thread_release(t, tirql);

end:
    scheduler_drop_two_locks(src, dst, sirql, dirql);
}
