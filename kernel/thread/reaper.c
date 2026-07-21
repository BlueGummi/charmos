#include <mem/alloc_or_die.h>
#include <sch/sched.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/workqueue.h>

static struct reaper_thread **reapers = NULL;
static atomic_size_t reaped_threads = ATOMIC_VAR_INIT(0);

void reaper_enqueue(struct thread *t) {
    kassert(reapers);
    size_t d = domain_local_id();
    locked_list_add(&reapers[d]->list, &t->reaper_list);
    semaphore_post(&reapers[d]->sem);
}

void reaper_init(void) {
    size_t reaper_count = global.domain_count;
    reapers = kmalloc_or_die(sizeof(struct reaper_thread *) * reaper_count,
                             ALLOC_FLAGS_ZERO);

    for (size_t i = 0; i < reaper_count; i++) {
        reapers[i] =
            alloc_or_die(kmalloc_from_domain(i, sizeof(struct reaper_thread)));

        locked_list_init(&reapers[i]->list, LOCKED_LIST_INIT_IRQ_DISABLE);
        semaphore_init(&reapers[i]->sem, 1, SEMAPHORE_INIT_IRQ_DISABLE);
        reapers[i]->thread = alloc_or_die(
            thread_create("reaper_thread", reaper_thread_main, NULL));

        domain_set_cpu_mask(&reapers[i]->thread->allowed_cpus,
                            global.domains[i]);
        reapers[i]->thread->private = reapers[i];
        thread_enqueue(reapers[i]->thread);
    }
}

size_t reaper_get_reaped_thread_count(void) {
    return atomic_load_explicit(&reaped_threads, memory_order_acquire);
}

void reaper_thread_main(void *unused) {
    (void) unused;
    struct reaper_thread *reaper = thread_get_current()->private;
    while (true) {

        while (locked_list_empty(&reaper->list))
            semaphore_wait(&reaper->sem);

        struct list_head local;
        INIT_LIST_HEAD(&local);

        enum irql tlist = spin_lock_irq_disable(&reaper->list.lock);
        list_splice_init(&reaper->list.list, &local);
        spin_unlock(&reaper->list.lock, tlist);

        struct list_head *lh;
        while ((lh = list_pop_front_init(&local)) != NULL) {
            struct thread *t = container_of(lh, struct thread, reaper_list);

            kassert(refcount_read(&t->refcount) == 0);
            thread_free(t);
            atomic_fetch_add(&reaped_threads, 1);
        }

        scheduler_yield();
    }
}
