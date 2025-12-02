#include <kassert.h>
#include <mem/alloc.h>
#include <mem/slab.h> /* to get SLAB_OBJ_ALIGN */
#include <sch/sched.h>
#include <sync/turnstile.h>

#include "mutex_internal.h"

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
    ts->waiter_max_prio = 0;
    ts->state = TURNSTILE_STATE_UNUSED;
    ts->inheritor = NULL;

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
        kmalloc(sizeof(struct turnstile), ALLOC_PARAMS_DEFAULT);
    if (!ts)
        return NULL;

    return turnstile_init(ts);
}

/* we don't have an official way of making a "priority" for a thread
 * because we combine priority classes and priority scoring in
 * thread scheduling. here, we take an approach where a given
 * number is a given priority with the timesharing class
 * having multiple numbers that the thread can get for its "priority" */

static inline struct turnstile_hash_chain *turnstile_chain_for(void *obj) {
    size_t idx = TURNSTILE_OBJECT_HASH(obj);
    return &global.turnstiles->heads[idx];
}

static void turnstile_insert_to_freelist(struct turnstile *parent,
                                         struct turnstile *child) {
    kassert(spinlock_held(&turnstile_chain_for(parent->lock_obj)->lock));
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
    kassert(spinlock_held(&chain->lock));
    list_add_tail(&chain->list, &ts->hash_list);
    ts->state = TURNSTILE_STATE_IN_HASH_TABLE;
    ts->lock_obj = lock_obj;
}

static void turnstile_remove(struct turnstile_hash_chain *chain,
                             struct turnstile *ts) {
    kassert(spinlock_held(&chain->lock));
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

static struct turnstile *turnstile_lookup_no_lock_for_hash_chain_internal(
    struct turnstile_hash_chain *except, void *obj, enum irql *irql_out,
    struct turnstile_hash_chain **thc_out) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);

    enum irql irql;

    if (chain != except)
        irql = turnstile_hash_chain_lock(chain);

    struct list_head *pos;
    struct turnstile *ts = NULL;

    list_for_each(pos, &chain->list) {
        if ((ts = turnstile_from_hash_list_node(pos))->lock_obj == obj)
            goto out;
    }

out:
    if (chain != except)
        *irql_out = irql;

    *thc_out = chain;

    return ts;
}

void turnstile_pi_remove(struct turnstile *ts) {
    if (ts->applied_pi_boost)
        scheduler_uninherit_priority();

    ts->applied_pi_boost = false;
}

struct thread *turnstile_dequeue_first(struct turnstile *ts, size_t queue) {
    void *obj = ts->lock_obj;
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);

    struct rbt_node *last = rb_last(&ts->queues[queue]);
    rb_delete(&ts->queues[queue], last);
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
    thread->blocked_on = NULL;

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
        thread_wake_manual(to_wake);
    }

    ts->inheritor = NULL;
    turnstile_hash_chain_unlock(chain, lock_irql);
}

void turnstile_unlock(void *obj, enum irql irql) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);
    turnstile_hash_chain_unlock(chain, irql);
}

void turnstile_propagate_boost(struct turnstile_hash_chain *locked_chain,
                               struct turnstile *ts, size_t waiter_weight,
                               enum thread_prio_class waiter_class) {
    /* again, do not swap me out while I do this dance */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    /* iterate and propagate upward */
    struct turnstile *cur_lock = ts;
    size_t boost_weight = waiter_weight;
    enum thread_prio_class boost_class = waiter_class;

    struct thread *original = ts->inheritor;

    while (true) {
        enum irql lock_irql;

        struct turnstile_hash_chain *this_chain = NULL;

        /* for that specific chain we do not attempt to acquire the lock, for
         * the lock is already held by the caller of this function. */
        struct turnstile *new_ts =
            turnstile_lookup_no_lock_for_hash_chain_internal(
                locked_chain, cur_lock->lock_obj, &lock_irql, &this_chain);

        /* A turnstile must exist if we have blocked on something */
        kassert(new_ts);

        struct thread *owner = cur_lock->inheritor;
        if (!owner)
            goto done;

        if (original == owner && ts != cur_lock)
            k_panic("Turnstile waiter cycle deadlock\n");

        /* if owner already has equal-or-higher effective inputs, stop */
        if (owner->weight >= boost_weight &&
            owner->perceived_prio_class <= boost_class)
            goto done;

        /* apply boost to owner */
        scheduler_inherit_priority(owner, boost_weight, boost_class);
        ts->applied_pi_boost = true; /* we gave you a boost */

        /* if owner is blocked on another lock, continue propagation */
        if ((cur_lock = owner->blocked_on)) {
            /* update boost values as the owner's effective prior`ity */
            boost_weight = owner->weight;
            boost_class = owner->perceived_prio_class;

            if (locked_chain != this_chain)
                turnstile_unlock(ts->lock_obj, lock_irql);

            continue;
        }

    done:
        if (locked_chain != this_chain)
            turnstile_unlock(ts->lock_obj, lock_irql);

        break;
    }

    irql_lower(irql);
}

static void turnstile_block_on(void *lock_obj, struct turnstile *ts,
                               size_t queue_num) {
    struct thread *curr = scheduler_get_current_thread();

    thread_block(curr, THREAD_BLOCK_REASON_MANUAL);
    rbt_insert(&ts->queues[queue_num], &curr->wq_tree_node);
    curr->blocked_on = lock_obj;
}

/* ok... the we first assign a turnstile to the lock object,
 * and then we boost priorities and finally block */
struct turnstile *turnstile_block(struct turnstile *ts, size_t queue_num,
                                  void *lock_obj, enum irql lock_irql) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(lock_obj);
    struct thread *current_thread = scheduler_get_current_thread();
    struct turnstile *my_turnstile = current_thread->turnstile;

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

    kassert(thread_get_state(current_thread) != THREAD_STATE_IDLE_THREAD);

    turnstile_propagate_boost(chain, ts, current_thread->weight,
                              current_thread->perceived_prio_class);

    turnstile_block_on(lock_obj, ts, queue_num);

    ts->waiters++;

    turnstile_hash_chain_unlock(chain, lock_irql);

    /* it is the waking thread's job to decrement waiters and
     * mark me as no longer being blocked on the lock object */

    scheduler_yield(); /* bye bye I'm blocked now... */

    return ts;
}

size_t turnstile_get_waiter_count(void *lock_obj) {
    size_t count = 0;
    struct turnstile *ts = turnstile_lookup_internal(lock_obj);
    if (ts)
        count = ts->waiters;

    return count;
}

void turnstile_set_inheritor(void *lobj, struct thread *t) {
    enum irql chain_lock;
    struct turnstile *ts;
    if ((ts = turnstile_lookup(lobj, &chain_lock)))
        ts->inheritor = t;

    turnstile_unlock(lobj, chain_lock);
}
