/* @title: IO waiting primitives */
#include <stdbool.h>
#include <thread/thread_types.h>
#include <structures/list.h>

struct io_wait_token {
    struct list_head list;
    struct thread *owner;
    void *wait_object;
    bool active;
    size_t magic;
};

enum io_wait_end_action {
    IO_WAIT_END_YIELD,
    IO_WAIT_END_NO_OP,
};

#define IO_WAIT_TOKEN_EMPTY                                                    \
    (struct io_wait_token){                                                    \
        .owner = NULL, .wait_object = NULL, .active = false, .magic = 0}

void io_wait_begin(struct io_wait_token *out, void *io_object);
void io_wait_end(struct io_wait_token *t, enum io_wait_end_action act);
static inline bool io_wait_token_active(struct io_wait_token *t) {
    return t->active;
}
