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
    thread_queue_push_back(&reaper.queue, t);
    condvar_signal(&reaper.cv);
}

void reaper_init(void) {
    thread_queue_init(&reaper.queue);
    condvar_init(&reaper.cv, CONDVAR_INIT_NORMAL);
    reaper_thread = thread_spawn("reaper_thread", reaper_thread_main, NULL);

    struct cpu_mask cmask;
    cpu_mask_init(&cmask, global.core_count);
    cpu_mask_set_all(&cmask);

    struct daemon_attributes attrs = {
        .max_timesharing_threads = 1,
        .thread_cpu_mask = cmask,
        .flags = DAEMON_FLAG_HAS_NAME,
    };
}

uint64_t reaper_get_reaped_thread_count(void) {
    return reaper.reaped_threads;
}

void reaper_thread_main(void *unused) {
    (void) unused;
    while (1) {
        enum irql irql = spin_lock(&reaper.lock);

        enum irql out;
        while (list_empty(&reaper.queue.list))
            condvar_wait(&reaper.cv, &reaper.lock, irql, &out);

        struct thread_queue local;
        thread_queue_init(&local);

        enum irql tlist = spin_lock_irq_disable(&reaper.queue.lock);
        list_splice_init(&reaper.queue.list, &local.list);
        spin_unlock(&reaper.queue.lock, tlist);

        spin_unlock(&reaper.lock, out);

        struct thread *t;
        while ((t = thread_queue_pop_front(&local)) != NULL) {

            if (refcount_read(&t->refcount) != 1) {
                thread_queue_push_back(&reaper.queue, t);
                break;
            }

            thread_set_state(t, THREAD_STATE_TERMINATED);
            thread_put(t);
            reaper.reaped_threads++;
        }

        scheduler_yield();
    }
}
