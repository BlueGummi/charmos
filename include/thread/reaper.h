/* @title: Thread reaper */
#pragma once
#include <structures/locked_list.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>
#include <thread/queue.h>
#include <thread/thread.h>

struct thread_reaper {
    struct locked_list list;
    struct spinlock lock;
    struct condvar cv;
    uint64_t reaped_threads;
};

void reaper_enqueue(struct thread *t);
void reaper_thread_main(void *nothing);
void reaper_init(void);
uint64_t reaper_get_reaped_thread_count(void);
void reaper_signal();
