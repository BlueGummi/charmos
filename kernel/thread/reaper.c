#include <sch/sched.h>
#include <thread/daemon.h>
#include <thread/defer.h>
#include <thread/reaper.h>

static struct daemon *reaper_daemon = NULL;
static struct thread_reaper reaper = {0};
static struct thread *reaper_thread = NULL;

void reaper_signal() {
    if (reaper_thread)
        condvar_signal(&reaper.cv);
}

void reaper_enqueue(struct thread *t) {
    locked_list_add(&reaper.list, &t->reaper_list);
    reaper_signal();
}

void reaper_init(void) {
    locked_list_init(&reaper.list);
    condvar_init(&reaper.cv, CONDVAR_INIT_NORMAL);
    reaper_thread = thread_spawn("reaper_thread", reaper_thread_main, NULL);
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}

void reaper_thread_main(void *unused) {
    (void) unused;
    while (true) {
        enum irql irql = spin_lock(&reaper.lock);

        while (locked_list_empty(&reaper.list))
            condvar_wait(&reaper.cv, &reaper.lock, irql, &irql);

        struct list_head local;
        INIT_LIST_HEAD(&local);

        enum irql tlist = spin_lock_irq_disable(&reaper.list.lock);
        list_splice_init(&reaper.list.list, &local);
        spin_unlock(&reaper.list.lock, tlist);

        spin_unlock(&reaper.lock, irql);

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
