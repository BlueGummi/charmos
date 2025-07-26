#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>

static struct thread_reaper reaper = {0};
static struct thread *reaper_thread = NULL;

void reaper_enqueue(struct thread *t) {
    bool i = spin_lock(&reaper.lock);

    t->next = NULL;
    t->prev = NULL;

    if (!reaper.queue.head) {
        reaper.queue.head = reaper.queue.tail = t;
    } else {
        reaper.queue.tail->next = t;
        t->prev = reaper.queue.tail;
        reaper.queue.tail = t;
    }

    condvar_signal(&reaper.cv);
    spin_unlock(&reaper.lock, i);
}

void reaper_init(void) {
    reaper_thread = thread_spawn(reaper_thread_main);
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}

void reaper_thread_main() {
    while (1) {
        bool i = spin_lock(&reaper.lock);

        while (!reaper.queue.head) {
            condvar_wait(&reaper.cv, &reaper.lock, i);
        }

        struct thread *t = reaper.queue.head;
        reaper.queue.head = reaper.queue.tail = NULL;

        spin_unlock(&reaper.lock, i);

        bool reaped_something = false;
        while (t) {
            atomic_store(&t->state, THREAD_STATE_TERMINATED);
            struct thread *next = t->next;
            reaper.reaped_threads++;
            thread_free(t);
            reaped_something = true;
            t = next;
        }

        if (reaped_something) {
            thread_sleep_for_ms(100);
        } else {
            thread_set_state(reaper_thread, THREAD_STATE_SLEEPING);
            scheduler_yield();
        }
    }
}
