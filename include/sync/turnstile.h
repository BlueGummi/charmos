#include <sch/thread_queue.h>
#include <stdatomic.h>

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
    struct thread *inheritor; /* who are we inheriting priority from? */
    struct list_head hash_list;
    struct list_head freelist;
    size_t waiters; /* how many goobers are blocking on me? */
    thread_prio_t waiter_max_prio;
    void *lock_obj; /* lock we are for */
    struct thread_queue queues[TURNSTILE_NUM_QUEUES];
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
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(turnstile_hash_chain, lock);

#define TURNSTILE_HASH_SIZE 128
#define TURNSTILE_HASH_MASK (TURNSTILE_HASH_SIZE - 1)

#define TURNSTILE_OBJECT_HASH(obj)                                             \
    ((((uintptr_t) (obj) >> 3) * 2654435761u) & TURNSTILE_HASH_MASK)

#define TURNSTILE_CHAIN(sobj) global.turnstiles[TURNSTILE_OBJECT_HASH(sobj)]

struct turnstile_hash_table {
    struct turnstile_hash_chain heads[TURNSTILE_HASH_SIZE];
};
