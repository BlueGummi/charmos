#include <sch/sched.h>
#include <thread/io_wait.h>
#include <thread/thread.h>

static atomic_size_t io_wait_magic = ATOMIC_VAR_INIT(0);

void io_wait_begin(struct io_wait_token *out, void *io_object) {
    struct thread *t = thread_get_current();

    *out = (struct io_wait_token) {.wait_object = io_object,
                                   .active = true,
                                   .magic = atomic_fetch_add(&io_wait_magic, 1),
                                   .owner = t};
    INIT_LIST_HEAD(&out->list);
    list_add_tail(&out->list, &t->io_wait_tokens);

    thread_block(t, THREAD_BLOCK_REASON_IO, THREAD_WAIT_UNINTERRUPTIBLE,
                 io_object);
}

void io_wait_end(struct io_wait_token *t, enum io_wait_end_action act) {
    struct thread *c = thread_get_current();
    bool found = false;

    /* double check */
    struct io_wait_token *iter;
    list_for_each_entry(iter, &c->io_wait_tokens, list) {
        kassert(iter->active);
        kassert(iter->owner == c);
        if (iter == t) {
            found = true;
            break;
        }
    }

    kassert(found);

    list_del(&t->list);

    if (list_empty(&c->io_wait_tokens)) {
        thread_unboost_self();

        if (act == IO_WAIT_END_YIELD)
            scheduler_yield();
    }

    t->active = false;
}
