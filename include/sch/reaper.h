/* @title: Thread reaper */
#pragma once
#include <sch/thread.h>
#include <sch/thread_queue.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

struct thread_reaper {
    struct thread_queue queue;
    struct spinlock lock;
    struct condvar cv;
    uint64_t reaped_threads;
};

void reaper_enqueue(struct thread *t);
void reaper_thread_main(void *nothing);
void reaper_init(void);
uint64_t reaper_get_reaped_thread_count(void);
