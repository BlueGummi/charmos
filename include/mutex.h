#include <sch/sched.h>
#include <sch/thread.h>
#include <spin_lock.h>

struct mutex {
    struct thread *owner;
    struct thread_queue waiters;
    struct spinlock lock;
};


