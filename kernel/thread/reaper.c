#include <sch/sched.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/workqueue.h>

static struct thread_reaper reaper = {0};
static struct thread *reaper_thread = NULL;

void reaper_signal() {
    if (reaper_thread)
        semaphore_post(&reaper.sem);
}

void reaper_enqueue(struct thread *t) {
    locked_list_add(&reaper.list, &t->reaper_list);
    reaper_signal();
}

void reaper_init(void) {
    locked_list_init(&reaper.list, LOCKED_LIST_INIT_IRQ_DISABLE);
    semaphore_init(&reaper.sem, 1, SEMAPHORE_INIT_IRQ_DISABLE);
    reaper_thread = thread_spawn("reaper_thread", reaper_thread_main, NULL);
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}

void reaper_thread_main(void *unused) {
    (void) unused;
    while (true) {

        while (locked_list_empty(&reaper.list))
            semaphore_wait(&reaper.sem);

        struct list_head local;
        INIT_LIST_HEAD(&local);

        enum irql tlist = spin_lock_irq_disable(&reaper.list.lock);
        list_splice_init(&reaper.list.list, &local);
        spin_unlock(&reaper.list.lock, tlist);

        struct list_head *lh;
        while ((lh = list_pop_front_init(&local)) != NULL) {
            struct thread *t = container_of(lh, struct thread, reaper_list);

            if (refcount_read(&t->refcount) != 1) {
                locked_list_add(&reaper.list, &t->reaper_list);
                break;
            }

            thread_set_state(t, THREAD_STATE_TERMINATED);
            thread_put(t);
            reaper.reaped_threads++;
        }

        scheduler_yield();
    }
}
