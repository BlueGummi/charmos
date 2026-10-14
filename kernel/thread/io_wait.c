#include <sch/sched.h>
#include <thread/io_wait.h>
#include <thread/thread.h>

void io_wait_begin(struct io_wait_token *out,
                   struct thread_wait_header *request) {
    struct thread *t = thread_get_current();
    enforce(!out->active);
    out->active = true;
    list_head_push_back(&t->io_wait_tokens, &out->list);

    thread_wait_header_init(request);
    const struct thread_wait_object object = {request, request};
    thread_wait_prepare(&object, 1, &out->block, THREAD_WAIT_UNINTERRUPTIBLE,
                        THREAD_BLOCK_REASON_IO);
}

void io_wait_complete(struct io_wait_token *t) {
    enforce(t->active && t->block.thread == thread_get_current());
    enforce(thread_get_current()->active_wait_blocks == &t->block);
    thread_wait_complete();
}

void io_wait_signal(struct thread_wait_header *request) {
    thread_wait_header_satisfy(request, THREAD_WAKE_REASON_BLOCKING_IO, NULL);
}

void io_wait_end(struct io_wait_token *t, enum io_wait_end_action act) {
    struct thread *c = thread_get_current();
    enforce(t->active && t->block.thread == c);

    /* submission failures can end registrations without yielding */
    if (c->active_wait_blocks == &t->block)
        thread_wait_cancel();

    list_node_delete_init(&t->list);
    t->active = false;

    if (list_head_empty(&c->io_wait_tokens)) {
        thread_unboost_self();
        if (act == IO_WAIT_END_YIELD)
            scheduler_yield();
    }
}
