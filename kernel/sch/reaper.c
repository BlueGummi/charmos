#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>

static struct thread_reaper reaper = {0};
static struct thread *reaper_thread = NULL;

void reaper_enqueue(struct thread *t) {
    enum irql irql = spin_lock_irq_disable(&reaper.lock);

    thread_queue_push_back(&reaper.queue, t);

    condvar_signal(&reaper.cv);

    spin_unlock(&reaper.lock, irql);
}

void reaper_init(void) {
    thread_queue_init(&reaper.queue);
    condvar_init(&reaper.cv);
    reaper_thread = thread_spawn(reaper_thread_main);
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}

void reaper_thread_main() {
    while (1) {
        enum irql irql = spin_lock_irq_disable(&reaper.lock);

        while (list_empty(&reaper.queue.list))
            condvar_wait(&reaper.cv, &reaper.lock, irql);

        struct list_head *l = list_pop_front(&reaper.queue.list);
        struct thread *t = thread_from_list_node(l);

        bool reaped_something = false;
        while (!list_empty(&reaper.queue.list)) {
            atomic_store(&t->state, THREAD_STATE_TERMINATED);
            struct list_head *l = list_pop_front(&reaper.queue.list);
            struct thread *next = thread_from_list_node(l);
            reaper.reaped_threads++;
            thread_put(t);
            reaped_something = true;
            t = next;
        }

        spin_unlock(&reaper.lock, irql);

        if (reaped_something) {
            thread_sleep_for_ms(100);
        } else {
            thread_sleep(t, THREAD_SLEEP_REASON_MANUAL);
            scheduler_yield();
        }
    }
}
