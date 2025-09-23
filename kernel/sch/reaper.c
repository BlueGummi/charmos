#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>

static struct thread_reaper reaper = {0};
static struct thread *reaper_thread = NULL;

void reaper_enqueue(struct thread *t) {
    thread_queue_push_back(&reaper.queue, t);
    condvar_signal(&reaper.cv);
}

void reaper_init(void) {
    thread_queue_init(&reaper.queue);
    condvar_init(&reaper.cv);
    reaper_thread = thread_spawn(reaper_thread_main);
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}

void reaper_thread_main(void) {
    while (1) {
        enum irql irql = spin_lock(&reaper.lock);

        while (list_empty(&reaper.queue.list))
            condvar_wait(&reaper.cv, &reaper.lock, irql);

        struct thread_queue local;
        thread_queue_init(&local);

        enum irql tlist = spin_lock_irq_disable(&reaper.queue.lock);
        list_splice_init(&reaper.queue.list, &local.list);
        spin_unlock(&reaper.queue.lock, tlist);

        spin_unlock(&reaper.lock, irql);

        bool reaped_something = false;
        struct thread *t;
        while ((t = thread_queue_pop_front(&local)) != NULL) {
            thread_set_state(t, THREAD_STATE_TERMINATED);
            reaper.reaped_threads++;
            thread_put(t);
            reaped_something = true;
        }

        if (reaped_something) {
            thread_sleep_for_ms(100);
        } else {
            scheduler_yield();
        }
    }
}
