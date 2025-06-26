#include <sch/sched.h>
#include <sch/thread.h>
#include <spin_lock.h>

struct mutex {
    struct thread *owner;
    struct thread_queue waiters;
    struct spinlock lock;
    bool initialized;
};
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
