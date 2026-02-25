#include <kassert.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <mem/slab.h> /* to get SLAB_OBJ_ALIGN */
#include <sch/sched.h>
#include <sync/turnstile.h>
#include <thread/thread.h>

#include "mutex_internal.h"

/* LOCK ORDERING: TS -> THREAD */

SLAB_SIZE_REGISTER_FOR_STRUCT(turnstile, SLAB_OBJ_ALIGN_DEFAULT);

/* Implements turnstiles used on synchronization objects
 *
 * This uses a strategy similar to later versions of Solaris
 * and FreeBSD, kudos to all the people who worked on those operating systems!
 *
 * Turnstiles give us pointer-sized adaptive mutexes (very good to have).
 *
 * Each thread is born with a turnstile (technically, this is still
 * *slightly* overkill because you need to have a thread to block on to even
 * use your turnstile, so it ideally would be n_threads/2 turnstiles on the
 * whole system, but that introduces a non-negligible amount of overhead for the
 * extra bookkeeping, so we just give each thread one turnstile and call it a
 * day).
 *
 * Whenever a thread blocks on a lock, we first go and check if the lock
 * already has an entry in the global turnstile hash table. If it is the first
 * thread to block on the lock (no entry in hash table), then we lend over our
 * own turnstile and go and block. If the lock already has a turnstile, then we
 * lend over our turnstile to the freelist of the turnstile that the lock is
 * associated with.
 *
 * When a thread wakes up from the block, it takes a turnstile from the freelist
 * of the turnstile that it is blocked on. If there are no waiters, then we
 * don't bother with that and just take the turnstile itself (there would be no
 * freelist) and remove it from the hash table.
 *
 * There is also priority inheritence which is done by walking the list of
 * blocked threads and taking our current thread's priority and boosting
 * threads that need the boost.
 *
 * The hash table per-head locks protect the hash tables adn contents of all
 * turnstiles residing in the hash table.
 */

void turnstiles_init() {
    global.turnstiles =
        kzalloc(sizeof(struct turnstile_hash_table), ALLOC_PARAMS_DEFAULT);
    if (!global.turnstiles)
        k_panic("Could not allocate turnstile hash table\n");
    for (size_t i = 0; i < TURNSTILE_HASH_SIZE; i++) {
        spinlock_init(&global.turnstiles->heads[i].lock);
        INIT_LIST_HEAD(&global.turnstiles->heads[i].list);
    }
}

#define TURNSTILE_BACKGROUND_PRIO 1
#define TURNSTILE_TS_PRIO_BASE 2
#define TURNSTILE_TS_PRIO_MAX 100000
#define TURNSTILE_RT_PRIO 100001
#define TURNSTILE_URGENT_PRIO 100002

static inline enum irql
turnstile_hash_chain_lock(struct turnstile_hash_chain *chain) {
    return spin_lock_irq_disable(&chain->lock);
}

static inline void
turnstile_hash_chain_unlock(struct turnstile_hash_chain *chain,
                            enum irql irql) {
    spin_unlock(&chain->lock, irql);
}

int32_t turnstile_thread_priority(struct thread *t) {
    switch (t->perceived_prio_class) {
    case THREAD_PRIO_CLASS_BACKGROUND: return TURNSTILE_BACKGROUND_PRIO;
    case THREAD_PRIO_CLASS_TIMESHARE:
        return (TURNSTILE_TS_PRIO_BASE + t->weight) > TURNSTILE_TS_PRIO_MAX
                   ? TURNSTILE_TS_PRIO_MAX
                   : TURNSTILE_TS_PRIO_BASE + t->weight;
    case THREAD_PRIO_CLASS_RT: return TURNSTILE_RT_PRIO;
    case THREAD_PRIO_CLASS_URGENT: return TURNSTILE_URGENT_PRIO;
    }
}

static size_t turnstile_thread_get_data(struct rbt_node *n) {
    return turnstile_thread_priority(thread_from_wq_rbt_node(n));
}

struct turnstile *turnstile_init(struct turnstile *ts) {
    ts->lock_obj = NULL;
    ts->waiters = 0;
    ts->state = TURNSTILE_STATE_UNUSED;
    ts->owner = NULL;

    rbt_init(&ts->queues[TURNSTILE_READER_QUEUE], turnstile_thread_get_data);
    rbt_init(&ts->queues[TURNSTILE_WRITER_QUEUE], turnstile_thread_get_data);
    INIT_LIST_HEAD(&ts->freelist);
    INIT_LIST_HEAD(&ts->hash_list);

    return ts;
}

void turnstile_destroy(struct turnstile *ts) {
    kfree(ts, FREE_PARAMS_DEFAULT);
}

struct turnstile *turnstile_create(void) {
    struct turnstile *ts =
        kzalloc(sizeof(struct turnstile), ALLOC_PARAMS_DEFAULT);
    if (!ts)
        return NULL;

    return turnstile_init(ts);
}

static inline struct turnstile_hash_chain *turnstile_chain_for(void *obj) {
    size_t idx = TURNSTILE_OBJECT_HASH(obj);
    return &global.turnstiles->heads[idx];
}

static void turnstile_insert_to_freelist(struct turnstile *parent,
                                         struct turnstile *child) {
    SPINLOCK_ASSERT_HELD(&turnstile_chain_for(parent->lock_obj)->lock);
    list_add_tail(&child->freelist, &parent->freelist);
    child->state = TURNSTILE_STATE_IN_FREE_LIST;
}

static struct turnstile *turnstile_freelist_pop(struct turnstile *ts) {
    struct list_head *lh = list_pop_front_init(&ts->freelist);
    kassert(lh); /* we are not to call this if the freelist is empty */
    struct turnstile *ret = turnstile_from_freelist(lh);
    ret->state = TURNSTILE_STATE_UNUSED;
    return ret;
}

static void turnstile_insert(struct turnstile_hash_chain *chain,
                             struct turnstile *ts, void *lock_obj) {
    SPINLOCK_ASSERT_HELD(&chain->lock);
    list_add_tail(&chain->list, &ts->hash_list);
    ts->state = TURNSTILE_STATE_IN_HASH_TABLE;
    ts->lock_obj = lock_obj;
}

static void turnstile_remove(struct turnstile_hash_chain *chain,
                             struct turnstile *ts) {
    SPINLOCK_ASSERT_HELD(&chain->lock);
    list_del_init(&ts->hash_list);
    ts->state = TURNSTILE_STATE_UNUSED;
    ts->lock_obj = NULL;
}

struct turnstile *turnstile_lookup_internal(void *obj) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);
    struct list_head *pos;

    struct turnstile *ts = NULL;
    list_for_each(pos, &chain->list) {
        if ((ts = turnstile_from_hash_list_node(pos))->lock_obj == obj)
            goto out;
    }

out:
    return ts;
}

struct turnstile *turnstile_lookup(void *obj, enum irql *irql_out) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);

    enum irql irql = turnstile_hash_chain_lock(chain);
    struct list_head *pos;
    struct turnstile *ts = NULL;

    list_for_each(pos, &chain->list) {
        if ((ts = turnstile_from_hash_list_node(pos))->lock_obj == obj)
            goto out;
    }

out:
    *irql_out = irql;
    return ts;
}

void turnstile_pi_remove(struct turnstile *ts) {
    if (ts->applied_pi_boost)
        thread_uninherit_priority(ts->prio_class);

    ts->prio_class = 0;
    ts->applied_pi_boost = false;
}

struct thread *turnstile_dequeue_first(struct turnstile *ts, size_t queue) {
    void *obj = ts->lock_obj;
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);

    struct rbt_node *last = rbt_last(&ts->queues[queue]);
    rbt_delete(&ts->queues[queue], last);
    struct thread *thread = thread_from_wq_rbt_node(last);

    struct turnstile *got = ts;
    if (ts->waiters == 1) { /* last waiter, take the turnstile with you! */
        /* you're taking the turnstile */
        kassert(list_empty(&ts->freelist));
        turnstile_remove(chain, ts);
    } else {
        /* not the last waiter, take one from the freelist */
        kassert(!list_empty(&ts->freelist));
        got = turnstile_freelist_pop(ts);
        kassert(got);
    }

    /* you take this turnstile with you as you wake up please */
    thread->turnstile = got;

    /* you are no longer blocked on a lock */
    atomic_store(&thread->blocked_ts, NULL);

    /* you are also no longer a waiter */
    ts->waiters--;
    return thread;
}

void turnstile_wake(struct turnstile *ts, size_t queue, size_t num_threads,
                    enum irql lock_irql) {
    /* remove from hash */
    void *obj = ts->lock_obj;
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);

    /* un-inherit the priority we inherited */
    turnstile_pi_remove(ts);

    /* yo, wake up */
    while (num_threads-- > 0) {
        /* wake the one of highest priority */
        struct thread *to_wake = turnstile_dequeue_first(ts, queue);
        thread_wake(to_wake, THREAD_WAKE_REASON_BLOCKING_MANUAL,
                    to_wake->perceived_prio_class, ts);
    }

    ts->owner = NULL;
    turnstile_hash_chain_unlock(chain, lock_irql);
}

void turnstile_unlock(void *obj, enum irql irql) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);
    turnstile_hash_chain_unlock(chain, irql);
}

void turnstile_propagate_boost(struct turnstile_hash_chain *locked_chain,
                               struct turnstile *ts) {
    struct turnstile *cur_ts = ts;
    struct thread *owner = NULL, *boosting_from = thread_get_current();

    while (cur_ts) {
        struct turnstile_hash_chain *chain =
            turnstile_chain_for(cur_ts->lock_obj);

        enum irql irql = IRQL_PASSIVE_LEVEL;
        bool unlock = false;

        if (chain != locked_chain) {
            irql = turnstile_hash_chain_lock(chain);
            unlock = true;
        }

        owner = cur_ts->owner;
        if (!owner) {
            if (unlock)
                turnstile_hash_chain_unlock(chain, irql);
            break;
        }

        /* Apply inheritance */
        if (!thread_inherit_priority(owner, boosting_from, NULL)) {
            if (unlock)
                turnstile_hash_chain_unlock(chain, irql);
            break;
        }

        cur_ts->applied_pi_boost = true;

        /* Speculative next hop */
        struct turnstile *next = atomic_load(&owner->blocked_ts);

        if (unlock)
            turnstile_hash_chain_unlock(chain, irql);

        /* Revalidation step */
        if (!next)
            break;

        struct turnstile_hash_chain *next_chain =
            turnstile_chain_for(next->lock_obj);

        enum irql nirql = turnstile_hash_chain_lock(next_chain);

        if (owner->blocked_ts != next || next->owner != owner) {
            turnstile_hash_chain_unlock(next_chain, nirql);
            break;
        }

        /* Prepare next iteration */
        boosting_from = owner;

        turnstile_hash_chain_unlock(next_chain, nirql);
        cur_ts = next;
    }
}

static void turnstile_block_on(struct turnstile *ts, size_t queue_num) {
    struct thread *curr = thread_get_current();

    atomic_store(&curr->blocked_ts, ts);

    thread_block(curr, THREAD_BLOCK_REASON_MANUAL, THREAD_WAIT_UNINTERRUPTIBLE,
                 ts);

    rbt_insert(&ts->queues[queue_num], &curr->wq_tree_node);
}

/* ok... the we first assign a turnstile to the lock object,
 * and then we boost priorities and finally block */

/* we already have preemption off when we get in here */
struct turnstile *turnstile_block(struct turnstile *ts, size_t queue_num,
                                  void *lock_obj, enum irql lock_irql,
                                  struct thread *owner) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(lock_obj);
    struct thread *current_thread = thread_get_current();

    struct turnstile *my_turnstile = current_thread->turnstile;

    kassert(my_turnstile);

    /* turnstile donation */
    if (!ts) {
        /* no turnstile to block on, give it ours */
        ts = my_turnstile;
        turnstile_insert(chain, ts, lock_obj);
        kassert(ts->waiters == 0);
    } else {
        /* someone else has donated a turnstile, put ours on the freelist */
        turnstile_insert_to_freelist(ts, my_turnstile);
        kassert(ts->waiters > 0);
        kassert(ts->lock_obj == lock_obj);
    }

    current_thread->turnstile = NULL;
    ts->owner = owner;

    turnstile_propagate_boost(chain, ts);

    ts->waiters++;

    turnstile_block_on(ts, queue_num);

    turnstile_hash_chain_unlock(chain, lock_irql);

    /* it is the waking thread's job to decrement waiters and
     * mark me as no longer being blocked on the lock object */
    thread_wait_for_wake_match();

    thread_remove_boost();

    return ts;
}

size_t turnstile_get_waiter_count(void *lock_obj) {
    size_t count = 0;
    struct turnstile *ts = turnstile_lookup_internal(lock_obj);
    if (ts)
        count = ts->waiters;

    return count;
}
