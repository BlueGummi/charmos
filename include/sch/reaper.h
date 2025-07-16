#pragma once
#include <sch/thread.h>
#include <spin_lock.h>

struct thread_reaper {
    struct thread_queue queue;
    struct spinlock lock;
    uint64_t reaped_threads;
};

void reaper_enqueue(struct thread *t);
void reaper_thread_main();
void reaper_init(void);
uint64_t reaper_get_reaped_thread_count(void);
