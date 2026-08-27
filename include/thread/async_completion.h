/* @title: Asynchronous completion objects */
#pragma once
#include <stdint.h>
#include <thread/apc.h>

enum async_completion_type {
    ASYNC_COMPLETION_APC = 0,
    ASYNC_COMPLETION_WORKER = 1,
};

/* TODO: I wrote this all out and never used it. this is a nice
 * wrapper around using APCs vs callbacks via worker for async
 * callbacks, and it would be great if I could start using the pattern */
struct async_completion {
    union {
        struct apc apc;

        /* We use this structure to store data BEFORE the
         * APC is enqueued. This is so that we can save
         * space in this very frequently instantiated structure,
         * and reuse space instead of using more */
        struct {
            apc_func_t apc_func_internal;
            void *ctx;

            /* NOTE: must match the position of *owner in struct apc.
             *
             * This is used here so that we don't use extra space             *
             * In prepare():
             *
             *    apc_init(&ac->apc);
             *
             * Later on...
             *    ac->type = type; // APC is '0', so it's fine to overwrite
             *                     // *owner
             *
             * In complete():
             *    enum async_completion_type type = ac->type;
             *
             *    check `type` and act on it */
            enum async_completion_type type;

            struct thread *thread;
        };
    };

    void (*callback)(struct async_completion *);
};

static_assert(offsetof(struct apc, owner) ==
              offsetof(struct async_completion, type));

static inline void async_complete(struct async_completion *ac, void *ctx) {
    if (ac->type == ASYNC_COMPLETION_APC) {
        struct thread *t = ac->thread;
        ac->thread = NULL;
        apc_enqueue(t, &ac->apc, APC_TYPE_KERNEL);
        apc_put(&ac->apc);
        thread_put(t);
    } else {
        ac->ctx = ctx;
        ac->callback(ac);
    }
}

static inline void async_apc_callback_internal(void *arg) {
    struct async_completion *ac = arg;
    ac->callback(ac);
}

static inline void async_prepare(
    struct async_completion *ac,
    void (*cb)(
        struct async_completion *), /* If type == ASYNC_COMPLETION_APC, this is
                                     * the APC callback. Otherwise, this is the
                                     * callback to wake up a worker */
    enum async_completion_type type) {
    if (type == ASYNC_COMPLETION_APC) {
        apc_init(&ac->apc, async_apc_callback_internal, ac, NULL);
        ac->thread = thread_get_current();
        kassert(thread_get(ac->thread));
    }

    ac->callback = cb;
    ac->type = type;
}

#define ASYNC_CTX_NONE NULL
