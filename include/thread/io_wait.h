/* @title: IO waiting primitives */
#pragma once
#include <thread/wait.h>

struct io_wait_token {
    struct thread_wait_block block;
    struct list_node list;
    bool active;
};

enum io_wait_end_action {
    IO_WAIT_END_YIELD,
    IO_WAIT_END_NO_OP,
};

#define IO_WAIT_TOKEN_EMPTY ((struct io_wait_token) {0})

void io_wait_begin(struct io_wait_token *out,
                   struct thread_wait_header *request);
void io_wait_end(struct io_wait_token *t, enum io_wait_end_action act);
void io_wait_complete(struct io_wait_token *t);
void io_wait_signal(struct thread_wait_header *request);
static inline bool io_wait_token_active(struct io_wait_token *t) {
    return t->active;
}
