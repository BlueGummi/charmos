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

    if (reaper_thread->state == SLEEPING)
        scheduler_wake(reaper_thread);

    spin_unlock(&reaper.lock, i);
}

void reaper_init(void) {
    reaper_thread = thread_spawn(reaper_thread_main);
}

void reaper_thread_main() {
    while (1) {
        bool i = spin_lock(&reaper.lock);

        struct thread *t = reaper.queue.head;
        reaper.queue.head = reaper.queue.tail = NULL;

        spin_unlock(&reaper.lock, i);

        bool reaped_something = false;
        while (t) {

            /* bye bye */
            t->state = TERMINATED;

            struct thread *next = t->next;
            thread_free(t);
            t = next;
            reaper.reaped_threads++;
            reaped_something = true;
        }

        if (reaped_something) {
            thread_sleep_for_ms(1000);
        } else {
            thread_set_state(reaper_thread, SLEEPING);
            scheduler_yield();
        }
    }
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}
