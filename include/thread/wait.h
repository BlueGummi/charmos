#pragma once

#include <structures/list.h>
#include <sync/spinlock.h>
#include <thread/thread_types.h>

#define THREAD_WAIT_BLOCK_SYNC 0
#define THREAD_NUM_WAIT_BLOCKS 4
#define THREAD_WAIT_KEY_NONE UINT16_MAX

struct thread_wait_header {
    struct spinlock lock;
    struct list_head waiters;
};

#define THREAD_WAIT_HEADER_INIT(name_)                                         \
    {.lock = SPINLOCK_INIT, .waiters = LIST_HEAD_INIT((name_).waiters)}

enum thread_wait_block_state {
    THREAD_WAIT_BLOCK_INACTIVE,
    THREAD_WAIT_BLOCK_ACTIVE,
    THREAD_WAIT_BLOCK_SATISFIED,
};

struct thread_wait_block {
    /* TODO: we're using a doubly linked list here because the block list under
     * a thread can grow arbitrarily long. Perhaps someday we can consider
     * something like Windows' MAXIMUM_WAIT_OBJECTS, so we can go for
     * a singularly linked list, save 8 bytes, at the cost of an
     * absolute maximum + maybe minor slowdowns (?) */
    struct list_node object_link;
    struct thread *thread;
    void *object;
    uint16_t key;
    enum thread_wait_block_state state;
    struct thread_wait_header *header;
    uint64_t epoch;
};

struct thread_wait_object {
    struct thread_wait_header *header;
    void *object;
};

struct thread_wait_result {
    enum thread_wait_status status;
    uint16_t key;
    enum thread_resume_reason reason;
};

void thread_wait_header_init(struct thread_wait_header *header);
void thread_wait_init(struct thread *thread);

/* If storage == NULL, we use the thread's embedded blocks. No allocation
 * ever happens in here, however, the caller remains runnable */
void thread_wait_prepare(const struct thread_wait_object *objects, size_t count,
                         struct thread_wait_block *storage,
                         enum thread_wait_type type,
                         enum thread_block_reason reason);

void thread_wait_prepare_one(struct thread_wait_header *header, void *object,
                             enum thread_wait_type type,
                             enum thread_block_reason reason);

void thread_wait_prepare_to_sleep(struct thread_wait_header *header,
                                  void *object, enum thread_wait_type type);

/* Satisfy unlinks ALL blocks before the waiter becomes runnable.
 * The callback runs just for the winner */
bool thread_wait_satisfy(struct thread_wait_block *block,
                         enum thread_resume_reason reason,
                         void (*callback)(struct thread *));

/* For callbacks that took a snapshot of the epoch */
bool thread_wait_satisfy_epoch(struct thread_wait_block *block, uint64_t epoch,
                               enum thread_resume_reason reason,
                               void (*callback)(struct thread *));

struct thread *thread_wait_header_satisfy(struct thread_wait_header *header,
                                          enum thread_resume_reason reason,
                                          void (*callback)(struct thread *));

/* Alert is a "level-triggered" singular event. We only end interruptible
 * waits with alerts, but otherwise, the alert will merely pend until
 * the next interruptible wait/park happens */
void thread_alert(struct thread *thread);
void thread_park(void);

/* The actual "go wait" action, yield and whatnot */
struct thread_wait_result thread_wait_complete(void);
void thread_wait_cancel(void);
