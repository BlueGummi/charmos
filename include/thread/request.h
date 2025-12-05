/* File defines "thread requests".
 *
 * Thread requests are used whenever a given subsystem has a need
 * to create a thread, but cannot create it with the `thread_create`
 * function due to a constraint or restriction.
 *
 * For example, memory allocators will never attempt to
 * dynamically call `thread_create`, as this can recursively
 * call the allocator and result in a potential unbounded
 * call stack growth and overflow.
 *
 * Thus, thread requests are used for thread creation, as opposed to a
 * `thread_create`. Thread requests are only to be used when the
 * presence of a thread is "beneficial, but not necessary" (e.g.
 * having an extra thread can help with parallelism and speed things up,
 * but is not integral to the integrity/functionality of a given subsystem).
 *
 * For example, a workqueue thread spawn can be performed with a "thread
 * request". This would basically mean "if any threads are destroyed
 * and become available, repurpose the memory used for them for these threads".
 *
 * Threads created from "thread requests" will only ever have stack
 * sizes that are at least the size of the default stack size (never smaller).
 *
 * "Thread requests" should only ever be statically allocated (e.g. embedded
 * inside of a structure), as making them dynamically allocated could
 * result in similar issues (unbounded stack growth).
 *
 * Thread request lists are instantiated as a per-domain structure, to help
 * with reducing lock contention and provide better NUMA locality when it
 * comes to the thread stacks being used for the different threads.
 *
 */

#pragma once
#include <containerof.h>
#include <stdatomic.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/locked_list.h>

struct thread; /* thread.h header is huge, just forward define */

enum thread_request_decision { /* What to do with the thread ? */
                               THREAD_REQUEST_DECISION_KEEP,
                               THREAD_REQUEST_DECISION_DESTROY,
};

/* callback to be used once a thread request is satisfied */
typedef enum thread_request_decision (*thread_request_callback)(struct thread *,
                                                                void *data);

enum thread_request_priority {
    THREAD_REQUEST_PRIORITY_LOW,
    THREAD_REQUEST_PRIORITY_HIGH,
    THREAD_REQUEST_PRIORITY_COUNT,
};

enum thread_request_state {
    THREAD_REQUEST_PENDING,
    THREAD_REQUEST_FULFILLED,
    THREAD_REQUEST_CANCELLED,
};

struct thread_request_list {
    struct locked_list lists[THREAD_REQUEST_PRIORITY_COUNT];
};

struct thread_request {
    struct list_head list_node;
    thread_request_callback callback;
    void (*thread_entry)(void *);
    void *arg;
    void *data;
    enum thread_request_priority prio;
    _Atomic enum thread_request_state state;

    /* No touchy pls */
    struct thread_request_list *_Atomic parent_internal;
};

#define THREAD_REQUEST_STATE(rq) atomic_load(&rq->state)

struct thread_request *thread_request_init(struct thread_request *rq);
void thread_request_enqueue(struct thread_request *request);
bool thread_request_cancel(struct thread_request *rq);

#define thread_request_from_list_node(ln)                                      \
    (container_of(ln, struct thread_request, list_node))
