#include <compiler/intrinsic.h>
#include <sch/sched.h>
#include <thread/apc.h>
#include <thread/thread.h>

#include "sch/internal.h"

/* Lock ordering here:
 *
 * wait_lock -> one wait_header.lock at a time, runqueues -> thread
 *
 * APCs can run a little bit when we're going to complete a wait start,
 * and alerts only unlink interruptible registrations
 */
static void deliver_alert_locked(struct thread *t);

void thread_wait_header_init(struct thread_wait_header *header) {
    spinlock_init(&header->lock);
    INIT_LIST_HEAD(&header->waiters);
}

void thread_wait_init(struct thread *t) {
    spinlock_init(&t->wait_lock);

    t->active_wait_blocks = NULL;
    t->wait_block_count = 0;
    t->wait_epoch = 0;
    t->wait_key = THREAD_WAIT_KEY_NONE;
    t->object_wait = false;
    t->alert_pending = false;

    for (size_t i = 0; i < THREAD_NUM_WAIT_BLOCKS; i++) {
        t->wait_blocks[i] = (struct thread_wait_block){0};
        INIT_LIST_HEAD(&t->wait_blocks[i].object_link);
    }
}

static void prepare_wait(const struct thread_wait_object *objects, size_t count,
                         struct thread_wait_block *storage,
                         enum thread_wait_type type, thread_act_reason_t reason,
                         enum thread_state state) {
    kassert(count <= UINT16_MAX);
    kassert(type != THREAD_WAIT_NONE);
    kassert(storage || count <= THREAD_NUM_WAIT_BLOCKS);

    struct thread *t = thread_get_current();
    if (!storage)
        storage = t->wait_blocks; /* TODO: establish identities for the 4
                                   * wait blocks, we'll take 0 for now */

    enum irql irql = spin_lock_high(&t->wait_lock);
    kassert(!t->active_wait_blocks);
    t->active_wait_blocks = storage;
    t->wait_block_count = count;
    t->wait_key = THREAD_WAIT_KEY_NONE;
    t->wait_done = false;
    t->wait_status = THREAD_WAIT_SATISFIED;
    t->wait_epoch++;
    t->wait_state = state;

    enum irql tirql = spin_lock_high(&t->lock);
    kassert(!t->object_wait);
    t->object_wait = true;
    atomic_store_rlx(&t->wait_type, type);
    thread_clear_flag(t, THREAD_FLAG_YIELDED);

    thread_diag_record_arm(t, "wait_blocks", ci_return_address(0),
                           count ? objects[0].object : NULL, state, type,
                           reason);

    /* Registration visible now, but we need to stay runnable so callers
     * can do their work before completeing */
    if (state == THREAD_STATE_SLEEPING) {
        thread_add_sleep_reason(t, reason);
    } else {
        thread_add_block_reason(t, reason);
    }

    thread_update_runtime_buckets(t, time_get_ms());

    spin_unlock(&t->lock, tirql);

    for (size_t i = 0; i < count; i++) {
        struct thread_wait_block *b = &storage[i];
        kassert(objects[i].header);
        *b = (struct thread_wait_block){.thread = t,
                                        .object = objects[i].object,
                                        .key = i,
                                        .state = THREAD_WAIT_BLOCK_ACTIVE,
                                        .header = objects[i].header,
                                        .epoch = t->wait_epoch};

        enum irql hirql = spin_lock_high(&b->header->lock);
        list_add_tail(&b->object_link, &b->header->waiters);
        spin_unlock(&b->header->lock, hirql);
    }

    deliver_alert_locked(t);
    spin_unlock(&t->wait_lock, irql);
}

void thread_wait_prepare(const struct thread_wait_object *objects, size_t count,
                         struct thread_wait_block *storage,
                         enum thread_wait_type type,
                         enum thread_block_reason reason) {
    prepare_wait(objects, count, storage, type, reason, THREAD_STATE_BLOCKED);
}

void thread_wait_prepare_to_sleep(struct thread_wait_header *header,
                                  void *object, enum thread_wait_type type) {
    const struct thread_wait_object obj = {header, object};
    prepare_wait(&obj, 1, NULL, type, THREAD_SLEEP_REASON_MANUAL,
                 THREAD_STATE_SLEEPING);
}

void thread_wait_prepare_one(struct thread_wait_header *header, void *object,
                             enum thread_wait_type type,
                             enum thread_block_reason reason) {
    const struct thread_wait_object obj = {header, object};
    thread_wait_prepare(&obj, 1, NULL, type, reason);
}

static void unlink_blocks(struct thread *t, uint16_t got) {
    for (size_t i = 0; i < t->wait_block_count; i++) {
        struct thread_wait_block *b = &t->active_wait_blocks[i];

        if (b->state != THREAD_WAIT_BLOCK_ACTIVE)
            continue;

        enum irql irql = spin_lock_high(&b->header->lock);

        list_del_init(&b->object_link);
        b->state =
            i == got ? THREAD_WAIT_BLOCK_SATISFIED : THREAD_WAIT_BLOCK_INACTIVE;

        spin_unlock(&b->header->lock, irql);
    }
}

static bool satisfy(struct thread *t, uint64_t epoch, uint16_t key,
                    enum thread_resume_reason reason,
                    void (*callback)(struct thread *)) {
    if (!t->active_wait_blocks || t->wait_epoch != epoch ||
        key >= t->wait_block_count ||
        t->active_wait_blocks[key].state != THREAD_WAIT_BLOCK_ACTIVE)
        return false;

    SPINLOCK_ASSERT_HELD(&t->wait_lock);
    unlink_blocks(t, key);
    t->wait_key = key;
    t->wait_done = true;
    t->wait_status = THREAD_WAIT_SATISFIED;
    t->wait_completion_reason = reason;

    if (callback)
        callback(t);

    scheduler_complete_object_wait(t, reason);
    return true;
}

bool thread_wait_satisfy(struct thread_wait_block *b,
                         enum thread_resume_reason reason,
                         void (*callback)(struct thread *)) {
    struct thread *t = b->thread;

    enum irql irql = spin_lock_high(&t->wait_lock);
    bool won = satisfy(t, b->epoch, b->key, reason, callback);
    spin_unlock(&t->wait_lock, irql);

    return won;
}

bool thread_wait_satisfy_epoch(struct thread_wait_block *b, uint64_t epoch,
                               enum thread_resume_reason reason,
                               void (*callback)(struct thread *)) {
    struct thread *t = b->thread;

    enum irql irql = spin_lock_high(&t->wait_lock);
    bool won = satisfy(t, epoch, b->key, reason, callback);
    spin_unlock(&t->wait_lock, irql);

    return won;
}

struct thread *thread_wait_header_satisfy(struct thread_wait_header *header,
                                          enum thread_resume_reason reason,
                                          void (*callback)(struct thread *)) {
    while (true) {
        enum irql irql = spin_lock_high(&header->lock);

        if (list_empty(&header->waiters)) {
            spin_unlock(&header->lock, irql);
            return NULL;
        }

        struct thread_wait_block *b = list_first_entry(
            &header->waiters, struct thread_wait_block, object_link);

        struct thread *t = b->thread;
        uint64_t epoch = b->epoch;
        uint16_t key = b->key;

        kassert(thread_get(t));
        spin_unlock(&header->lock, irql);

        irql = spin_lock_high(&t->wait_lock);
        bool won = satisfy(t, epoch, key, reason, callback);
        spin_unlock(&t->wait_lock, irql);

        thread_put(t);
        if (won)
            return t;
    }
}

static void finish_locked(struct thread *t) {
    t->active_wait_blocks = NULL;
    t->wait_block_count = 0;

    enum irql irql = spin_lock_high(&t->lock);

    t->object_wait = false;
    atomic_store_rlx(&t->wait_type, THREAD_WAIT_NONE);
    enum thread_state state = thread_get_state(t);

    if (state == THREAD_STATE_BLOCKED || state == THREAD_STATE_SLEEPING)
        thread_set_state(t, THREAD_STATE_RUNNING);

    spin_unlock(&t->lock, irql);
}

struct thread_wait_result thread_wait_complete(void) {
    struct thread *t = thread_get_current();

    /* Whether the previous loop ran any APCs. We need this because
     * a delivery pass would leave the thread RUNNING. The
     * pass after one that delivers has to go actually block. */
    bool delivered = false;

    while (true) {
        enum irql irql = spin_lock_high(&t->wait_lock);
        kassert(t->active_wait_blocks);

        if (t->wait_done) {
            struct thread_wait_result result = {.status = t->wait_status,
                                                .key = t->wait_key,
                                                .reason =
                                                    t->wait_completion_reason};
            finish_locked(t);
            spin_unlock(&t->wait_lock, irql);
            return result;
        }

        enum irql tirql = spin_lock_high(&t->lock);

        bool apc =
            atomic_load_rlx(&t->wait_type) == THREAD_WAIT_INTERRUPTIBLE &&
            !delivered && atomic_load(&t->apc_pending_mask) != 0;

        thread_clear_flag(t, THREAD_FLAG_YIELDED);

        thread_set_state(t, apc ? THREAD_STATE_RUNNING : t->wait_state);

        thread_update_runtime_buckets(t, time_get_ms());

        spin_unlock(&t->lock, tirql);
        spin_unlock(&t->wait_lock, irql);

        if (apc)
            apc_check_and_deliver(t);

        delivered = apc;

        scheduler_yield();
    }
}

void thread_wait_cancel(void) {
    struct thread *t = thread_get_current();
    enum irql irql = spin_lock_high(&t->wait_lock);
    kassert(t->active_wait_blocks);
    unlink_blocks(t, THREAD_WAIT_KEY_NONE);
    finish_locked(t);
    spin_unlock(&t->wait_lock, irql);
}

static void deliver_alert_locked(struct thread *t) {
    if (!t->alert_pending || !t->active_wait_blocks || t->wait_done ||
        atomic_load_rlx(&t->wait_type) != THREAD_WAIT_INTERRUPTIBLE)
        return;

    unlink_blocks(t, THREAD_WAIT_KEY_NONE);

    t->alert_pending = false;
    t->wait_done = true;
    t->wait_status = THREAD_WAIT_ALERTED;
    t->wait_key = THREAD_WAIT_KEY_NONE;
    t->wait_completion_reason = t->wait_state == THREAD_STATE_SLEEPING
                                    ? THREAD_WAKE_REASON_SLEEP_MANUAL
                                    : THREAD_WAKE_REASON_BLOCKING_MANUAL;

    scheduler_complete_object_wait(t, t->wait_completion_reason);
}

void thread_alert(struct thread *t) {
    kassert(t);
    enum irql irql = spin_lock_high(&t->wait_lock);

    if (!thread_test_flag(t, THREAD_FLAG_DYING)) {
        t->alert_pending = true;
        deliver_alert_locked(t);
    }

    spin_unlock(&t->wait_lock, irql);
}

void thread_park(void) {
    prepare_wait(/*objects=*/NULL, /*count=*/0, /*storage=*/NULL,
                 THREAD_WAIT_INTERRUPTIBLE, THREAD_SLEEP_REASON_MANUAL,
                 THREAD_STATE_SLEEPING);
    thread_wait_complete();
}
