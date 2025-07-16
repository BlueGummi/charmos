#include <sch/thread.h>
#include <spin_lock.h>

struct thread_reaper {
    struct thread_queue queue;
    struct spinlock lock;
};
void reaper_enqueue(struct thread *t);
void reaper_thread_main();
void reaper_init(void);
