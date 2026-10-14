#include <enforce.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <mem/nofail.h>
#include <mem/slab.h> /* to get SLAB_OBJ_ALIGN */
#include <sch/sched.h>
#include <sync/rcu.h>
#include <sync/turnstile.h>
#include <thread/thread.h>

/* LOCK ORDERING: chain -> thread.wait_lock -> header / runqueues -> thread */

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
 * use your turnstile, so it ideally would be thread_count/2 turnstiles on the
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
 * The hash table per-head locks protect the hash tables and contents of all
 * turnstiles residing in the hash table.
 */

LOCK_CHK_CLASS_DEFINE_LOCAL(turnstile_chain);

void turnstiles_init(void) {
    global.turnstiles = nofail_vnew(global.turnstiles, ALLOC_ZERO);
    for (size_t i = 0; i < TURNSTILE_HASH_SIZE; i++) {
        spinlock_init_chk(&global.turnstiles->heads[i].lock,
                          LOCK_CHK_CLASS(turnstile_chain), LOCK_CHKD_FULL);
        INIT_LIST_HEAD(&global.turnstiles->heads[i].list);
    }
}

#define TURNSTILE_BACKGROUND_PRIO 1
#define TURNSTILE_TS_PRIO_BASE 2
#define TURNSTILE_TS_PRIO_MAX 100000
#define TURNSTILE_RT_PRIO 100001
#define TURNSTILE_URGENT_PRIO 100002

static inline enum irql
turnstile_hash_chain_lock(struct turnstile_hash_chain *chain) TSA_NO_ANALYSIS {
    return spin_lock_high(&chain->lock);
}

static inline void
turnstile_hash_chain_unlock(struct turnstile_hash_chain *chain,
                            enum irql irql) TSA_NO_ANALYSIS {
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
    unreachable("thread prio class invalid");
}

/* NOTE: we copy the computed turnstile wait queue key and then put it
 * in wq_prio. This is because threads can change the actual priority (class,
 * or the weights for timesharing threads) without holding the hash
 * chain lock, which would lead to keys in the tree shifting about,
 * and some bigger problems for other readers.
 *
 * We do eventually turnstile_requeue_waiter though. */
static size_t turnstile_thread_get_data(struct rbt_node *n) {
    return thread_from_wq_rbt_node(n)->wq_prio;
}

static int32_t turnstile_thread_cmp(const struct rbt_node *a,
                                    const struct rbt_node *b) {
    int32_t ta = turnstile_thread_get_data((void *) a);
    int32_t tb = turnstile_thread_get_data((void *) b);
    return ta - tb;
}

struct turnstile *turnstile_init(struct turnstile *ts) {
    thread_wait_header_init(&ts->wait);
    ts->lock_obj = NULL;
    ts->waiters = 0;
    ts->state = TURNSTILE_STATE_UNUSED;
    ts->owner = NULL;

    rbt_init(&ts->queues[TURNSTILE_READER_QUEUE], turnstile_thread_get_data,
             turnstile_thread_cmp);
    rbt_init(&ts->queues[TURNSTILE_WRITER_QUEUE], turnstile_thread_get_data,
             turnstile_thread_cmp);
    INIT_LIST_HEAD(&ts->freelist);
    INIT_LIST_HEAD(&ts->hash_list);

    return ts;
}

void turnstile_destroy(struct turnstile *ts) {
    vfree(ts);
}

struct turnstile *turnstile_create(void) {
    struct turnstile *ts = vnew(ts, ALLOC_ZERO);
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
    enforce(lh); /* we are not to call this if the freelist is empty */
    struct turnstile *ret = turnstile_from_freelist(lh);
    ret->state = TURNSTILE_STATE_UNUSED;
    return ret;
}

static void turnstile_insert(struct turnstile_hash_chain *chain,
                             struct turnstile *ts, void *lock_obj) {
    SPINLOCK_ASSERT_HELD(&chain->lock);
    list_add_tail(&ts->hash_list, &chain->list);
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

struct turnstile_wait_snapshot {
    struct turnstile *ts;
    void *lock_obj;
    uint64_t epoch;
};

static bool turnstile_snapshot_wait(struct thread *thread,
                                    struct turnstile_wait_snapshot *wait) {
    enum irql irql = spin_lock_high(&thread->wait_lock);
    struct thread_wait_block *block = thread->active_wait_blocks;
    bool valid =
        block == thread->wait_blocks &&
        thread->wait_block_count > THREAD_WAIT_BLOCK_SYNC &&
        block[THREAD_WAIT_BLOCK_SYNC].state == THREAD_WAIT_BLOCK_ACTIVE;

    if (valid) {
        wait->ts = atomic_load_rlx(&thread->blocked_ts);
        wait->lock_obj = block[THREAD_WAIT_BLOCK_SYNC].object;
        wait->epoch = block[THREAD_WAIT_BLOCK_SYNC].epoch;
        valid = wait->ts != NULL;
    }

    spin_unlock(&thread->wait_lock, irql);
    return valid;
}

static void turnstile_requeue_waiter(struct turnstile *ts,
                                     struct thread *thread) {
    SPINLOCK_ASSERT_HELD(&turnstile_chain_for(ts->lock_obj)->lock);

    int32_t prio = turnstile_thread_priority(thread);
    if (thread->wq_prio == prio)
        return;

    for (size_t queue = 0; queue < TURNSTILE_NUM_QUEUES; queue++) {
        struct rbt *tree = &ts->queues[queue];
        if (!rbt_has_node(tree, &thread->wq_tree_node))
            continue;

        thread->wq_prio = prio;
        rbt_reinsert(tree, &thread->wq_tree_node);
        return;
    }
}

static struct turnstile *turnstile_lookup_internal(void *obj) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);
    struct list_head *pos;

    list_for_each(pos, &chain->list) {
        struct turnstile *ts = turnstile_from_hash_list_node(pos);
        if (ts->lock_obj == obj)
            return ts;
    }

    return NULL;
}

enum irql turnstile_lookup(void *obj, struct turnstile **out_ts) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);

    enum irql irql = turnstile_hash_chain_lock(chain);
    if (out_ts)
        *out_ts = turnstile_lookup_internal(obj);
    return irql;
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

    atomic_store_rlx(&thread->blocked_ts, NULL);

    struct turnstile *got = ts;
    if (ts->waiters == 1) { /* last waiter, take the turnstile with you! */
        /* you're taking the turnstile */
        enforce(list_empty(&ts->freelist));
        turnstile_remove(chain, ts);
    } else {
        /* not the last waiter, take one from the freelist */
        enforce(!list_empty(&ts->freelist));
        got = enforce(turnstile_freelist_pop(ts));
    }

    /* you take this turnstile with you as you wake up please */
    thread->turnstile = got;

    /* you are also no longer a waiter */
    ts->waiters--;
    return thread;
}

void turnstile_wake(struct turnstile *ts, size_t queue, size_t num_threads,
                    enum irql lock_irql) {
    /* remove from hash */
    void *obj = ts->lock_obj;
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);
    SPINLOCK_ASSERT_HELD(&chain->lock);

    /* un-inherit the priority we inherited */
    turnstile_pi_remove(ts);

    ts->owner = NULL;

    /* yo, wake up */
    while (num_threads-- > 0) {
        /* wake the one of highest priority */
        struct thread *to_wake = turnstile_dequeue_first(ts, queue);
        enforce(
            thread_wait_satisfy(&to_wake->wait_blocks[THREAD_WAIT_BLOCK_SYNC],
                                THREAD_WAKE_REASON_BLOCKING_MANUAL, NULL));
    }

    turnstile_hash_chain_unlock(chain, lock_irql);
}

void turnstile_unlock(void *obj, enum irql irql) {
    struct turnstile_hash_chain *chain = turnstile_chain_for(obj);
    turnstile_hash_chain_unlock(chain, irql);
}

static enum irql
turnstile_propagate_boost(struct turnstile_hash_chain *locked_chain,
                          struct turnstile *ts, enum irql irql) {
    struct turnstile_hash_chain *chain = locked_chain;
    struct turnstile *cur_ts = ts;
    struct thread *boosting_from = thread_get_current();

    /* RCU keeps threads alive when reading owners,
     * and we re-hash objects upon lookup */
    rcu_read_lock();

    while (true) {
        struct thread *owner = cur_ts->owner;
        if (!owner)
            break;

        enum thread_prio_class old_class;

        /* If it fails, that means the owner did not inherit our
         * priority. Thus, we must short circuit the walk */
        if (!thread_inherit_priority(owner, boosting_from, &old_class))
            break;

        if (!cur_ts->applied_pi_boost) {
            cur_ts->prio_class = old_class;
            cur_ts->applied_pi_boost = true;
        }

        struct turnstile_wait_snapshot next_wait, current_wait;
        if (!turnstile_snapshot_wait(owner, &next_wait))
            break;

        struct turnstile_hash_chain *next_chain =
            turnstile_chain_for(next_wait.lock_obj);

        if (next_chain != chain) {
            turnstile_hash_chain_unlock(chain, irql);
            chain = next_chain;
            irql = turnstile_hash_chain_lock(chain);
        }

        /* Lookup again */
        cur_ts = turnstile_lookup_internal(next_wait.lock_obj);

        bool ts_valid = cur_ts == next_wait.ts;
        bool snapshot = turnstile_snapshot_wait(owner, &current_wait);
        bool curr_valid = snapshot && current_wait.ts == cur_ts &&
                          current_wait.epoch == next_wait.epoch;

        if (!ts_valid || !curr_valid)
            break;

        turnstile_requeue_waiter(cur_ts, owner);

        boosting_from = owner;
    }

    if (chain != locked_chain) {
        turnstile_hash_chain_unlock(chain, irql);
        irql = turnstile_hash_chain_lock(locked_chain);
    }

    rcu_read_unlock();
    return irql;
}

static void turnstile_block_on(struct turnstile *ts, size_t queue_num) {
    struct thread *curr = thread_get_current();

    atomic_store_rlx(&curr->blocked_ts, ts);

    thread_wait_prepare_one(&ts->wait, ts->lock_obj,
                            THREAD_WAIT_UNINTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);

    curr->wq_prio = turnstile_thread_priority(curr);
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

    enforce(my_turnstile);

    /* turnstile donation */
    if (!ts) {
        /* no turnstile to block on, give it ours */
        ts = my_turnstile;
        turnstile_insert(chain, ts, lock_obj);
        enforce(ts->waiters == 0);
    } else {
        /* someone else has donated a turnstile, put ours on the freelist */
        turnstile_insert_to_freelist(ts, my_turnstile);
        enforce(ts->waiters > 0);
        enforce(ts->lock_obj == lock_obj);
    }

    current_thread->turnstile = NULL;
    ts->owner = owner;

    ts->waiters++;

    turnstile_block_on(ts, queue_num);

    lock_irql = turnstile_propagate_boost(chain, ts, lock_irql);

    turnstile_hash_chain_unlock(chain, lock_irql);

    /* it is the waking thread's job to decrement waiters */
    thread_wait_complete();

    thread_remove_boost();

    return current_thread->turnstile;
}

size_t turnstile_get_waiter_count(void *lock_obj) {
    struct turnstile *ts = turnstile_lookup_internal(lock_obj);
    return ts ? ts->waiters : 0;
}
