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

    spin_unlock(&reaper.lock, i);
}

static void wake_reaper(void *arg) {
    (void) arg;
    scheduler_wake(reaper_thread);
}

void reaper_init(void) {
    struct thread *t = thread_create(reaper_thread_main);
    scheduler_enqueue(t);
    reaper_thread = t;
}

void reaper_thread_main() {
    while (1) {
        bool i = spin_lock(&reaper.lock);

        struct thread *t = reaper.queue.head;
        reaper.queue.head = reaper.queue.tail = NULL;

        spin_unlock(&reaper.lock, i);

        while (t) {

            /* bye bye */
            t->state = TERMINATED;
            t->prev = NULL;
            t->next = NULL;

            struct thread *next = t->next;
            thread_free(t);
            t = next;
        }

        disable_interrupts();
        reaper_thread->state = SLEEPING;

        void *arg = NULL;
        uint64_t check_ms = 1000;
        defer_enqueue(wake_reaper, arg, check_ms);
        enable_interrupts();

        scheduler_yield();
    }
}
