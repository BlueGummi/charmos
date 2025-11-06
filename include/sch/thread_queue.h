#pragma once
#include <structures/list.h>
#include <sync/spinlock.h>

struct thread_queue {
    struct list_head list;
    struct spinlock lock;
};

void thread_queue_init(struct thread_queue *q);
void thread_queue_push_back(struct thread_queue *q, struct thread *t);
void thread_block_on(struct thread_queue *q);
struct thread *thread_queue_pop_front(struct thread_queue *q);
void thread_queue_clear(struct thread_queue *q);
bool thread_queue_remove(struct thread_queue *q, struct thread *t);
