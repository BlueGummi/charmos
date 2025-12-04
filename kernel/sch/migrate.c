#include "internal.h"

/* file implements `thread_migrate`.
*
* NOTE: when we acquire two `struct scheduler` locks,
* we ALWAYS ALWAYS acquire the lock for the LOWER 
* memory address first, same for unlock, 
* and we ALWAYS acquire thread
* locks AFTER any scheduler locks! */

void thread_migrate(struct thread *t, size_t dest_core) {
    /* first acquire both the lock of the thread's scheduler
     * and the destination core's scheduler */

    enum thread_flags flags;
    struct scheduler *src = global.schedulers[thread_get_last_ran(t, &flags)];
    struct scheduler *dst = global.schedulers[dest_core];

    enum irql sirql, dirql, tirql;
    kassert(src != dst);

    if (src < dst) {
        sirql = scheduler_lock_irq_disable(src);
        dirql = scheduler_lock_irq_disable(dst);
    } else {
        dirql = scheduler_lock_irq_disable(dst);
        sirql = scheduler_lock_irq_disable(src);
    }
    tirql = thread_acquire(t);

    
    /* finally, we can do something here. */


out:

    thread_release(t, tirql);
    if (src < dst) {
        scheduler_unlock(src, sirql);
        scheduler_unlock(dst, dirql);
    } else {
        scheduler_unlock(dst, dirql);
        scheduler_unlock(src, sirql);
    }

    thread_set_flags(t, flags);
}
