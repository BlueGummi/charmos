#include <stdatomic.h>
#include <structures/pairing_heap.h>
#include <structures/rbt.h>
#include <thread/queue.h>
#include <thread/thread_types.h>

#define TURNSTILE_WRITER_QUEUE 0
#define TURNSTILE_READER_QUEUE 1
#define TURNSTILE_NUM_QUEUES 2

enum turnstile_state {
    TURNSTILE_STATE_UNUSED, /* we are unused by anything... */

    TURNSTILE_STATE_IN_HASH_TABLE, /* we are the turnstile
                                    * responsible for some lock
                                    * and we are in the hash table */

    TURNSTILE_STATE_IN_FREE_LIST, /* we are sitting on the freelist of
                                   * a turnstile sitting around in the
                                   * hash table of turnstiles */
};

struct turnstile {
    struct thread *owner;

    /* If a boost occurs, what did we give it? */
    bool applied_pi_boost;
    enum thread_prio_class prio_class;
    struct list_head hash_list;
    struct list_head freelist;
    size_t waiters; /* how many goobers are blocking on me? */
    void *lock_obj; /* lock we are for */
    struct rbt queues[TURNSTILE_NUM_QUEUES];
    enum turnstile_state state;
};

#define turnstile_from_freelist(fl)                                            \
    (container_of(fl, struct turnstile, freelist))
#define turnstile_from_hash_list_node(hln)                                     \
    (container_of(hln, struct turnstile, hash_list))

/* chains in hashtable */
struct turnstile_hash_chain {
    struct list_head list;
    struct spinlock lock;
};

#define TURNSTILE_HASH_SIZE 128
#define TURNSTILE_HASH_MASK (TURNSTILE_HASH_SIZE - 1)

#define TURNSTILE_OBJECT_HASH(obj)                                             \
    ((((uintptr_t) (obj) >> 3) * 2654435761u) & TURNSTILE_HASH_MASK)

#define TURNSTILE_CHAIN(sobj) global.turnstiles[TURNSTILE_OBJECT_HASH(sobj)]

struct turnstile_hash_table {
    struct turnstile_hash_chain heads[TURNSTILE_HASH_SIZE];
};

void turnstiles_init();
struct turnstile *turnstile_create(void);
void turnstile_destroy(struct turnstile *ts);
struct turnstile *turnstile_init(struct turnstile *ts);
struct turnstile *turnstile_block(struct turnstile *ts, size_t queue_num,
                                  void *lock_obj, enum irql lock_irql,
                                  struct thread *owner);
struct turnstile *turnstile_lookup(void *obj, enum irql *irql_out);
void turnstile_unlock(void *obj, enum irql irql);
void turnstile_wake(struct turnstile *ts, size_t queue, size_t num_threads,
                    enum irql lock_irql);
size_t turnstile_get_waiter_count(void *lock_obj);
int32_t turnstile_thread_priority(struct thread *t);
