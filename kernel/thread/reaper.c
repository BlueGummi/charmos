#include <mem/alloc.h>
#include <mem/nofail.h>
#include <sch/sched.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/workqueue.h>

static struct reaper_thread **reapers = NULL;
static atomic_size_t reaped_threads = ATOMIC_VAR_INIT(0);

void reaper_enqueue(struct thread *t) {
    enforce(reapers);
    size_t d = domain_local_id(TOPC_NONE);
    locked_list_add(&reapers[d]->list, &t->reaper_list);
    semaphore_post(&reapers[d]->sem);
}

void reaper_init(void) {
    size_t reaper_count = global.domain_count;
    reapers = nofail_vnews(reapers, reaper_count, ALLOC_ZERO);

    for (size_t i = 0; i < reaper_count; i++) {
        reapers[i] =
            nofail(valloc_from_domain(i, sizeof(struct reaper_thread)));

        locked_list_init(&reapers[i]->list, LOCKED_LIST_INIT_IRQ_DISABLE);
        semaphore_init(&reapers[i]->sem, 1, SEMAPHORE_INIT_IRQ_DISABLE);
        reapers[i]->thread = nofail(thread_spawn(
            "reaper_thread", reaper_thread_main, .private = reapers[i],
            .allowed_cpus = global.domains[i]->cpu_mask));
    }
}

size_t reaper_get_reaped_thread_count(void) {
    return atomic_load_acq(&reaped_threads);
}

void reaper_thread_main(void *unused) {
    cc_unused(unused);
    struct reaper_thread *reaper = thread_get_current()->private;
    while (true) {

        while (locked_list_empty(&reaper->list))
            semaphore_wait(&reaper->sem);

        struct list_head local;
        list_head_init(&local);

        enum irql tlist = spin_lock_high(&reaper->list.lock);
        list_head_splice_back_init(&local, &reaper->list.list);
        spin_unlock(&reaper->list.lock, tlist);

        struct list_node *lh;
        while ((lh = list_head_pop_front_init(&local)) != NULL) {
            struct thread *t = container_of(lh, struct thread, reaper_list);

            enforce(refcount_read(&t->refcount) == 0);
            thread_free(t);
            atomic_inc(&reaped_threads);
        }

        scheduler_yield();
    }
}
