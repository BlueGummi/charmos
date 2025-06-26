#include <sch/sched.h>
#include <sch/thread.h>

struct mutex {
    struct thread *owner;
    struct thread_queue waiters;
};


