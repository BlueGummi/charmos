#include "thread/tests/test_internal.h"
#include <thread/io_wait.h>

TEST_GROUP_DEFINE(wait_block);

TEST_DEFINE_UNIT(wait_block, caller_storage_wait_any) {
    struct thread_wait_header headers[6];
    struct thread_wait_object objects[6];
    struct thread_wait_block blocks[6];
    for (size_t i = 0; i < 6; i++) {
        thread_wait_header_init(&headers[i]);
        objects[i] = (struct thread_wait_object){&headers[i], &headers[i]};
    }
    thread_wait_prepare(objects, 6, blocks, THREAD_WAIT_UNINTERRUPTIBLE,
                        THREAD_BLOCK_REASON_MANUAL);
    TEST_ASSERT_EQ(thread_get_current()->active_wait_blocks, &blocks[0]);
    TEST_ASSERT(thread_wait_satisfy(&blocks[4],
                                    THREAD_WAKE_REASON_BLOCKING_MANUAL, NULL));
    for (size_t i = 0; i < 6; i++) {
        TEST_ASSERT(list_head_empty(&headers[i].waiters));
        TEST_ASSERT_EQ(blocks[i].state, i == 4 ? THREAD_WAIT_BLOCK_SATISFIED
                                               : THREAD_WAIT_BLOCK_INACTIVE);
    }
    TEST_ASSERT_EQ(thread_wait_complete().key, 4);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(wait_block, io_boost_and_cancel) {
    struct thread_wait_header request;
    struct io_wait_token token = IO_WAIT_TOKEN_EMPTY;
    struct thread *t = thread_get_current();
    enum thread_prio_class base = t->base_prio_class;

    io_wait_begin(&token, &request);
    io_wait_signal(&request);
    io_wait_complete(&token);
    TEST_ASSERT_EQ(t->perceived_prio_class, THREAD_PRIO_CLASS_URGENT);
    TEST_ASSERT(io_wait_token_active(&token));
    TEST_ASSERT(list_head_empty(&request.waiters));
    io_wait_end(&token, IO_WAIT_END_NO_OP);
    TEST_ASSERT_EQ(t->perceived_prio_class, base);
    TEST_ASSERT(!io_wait_token_active(&token));

    /* Submission failed before yield and cancellation must restore RUNNING */
    io_wait_begin(&token, &request);
    io_wait_end(&token, IO_WAIT_END_NO_OP);
    TEST_ASSERT(list_head_empty(&request.waiters));
    TEST_ASSERT_EQ(thread_get_state(t), THREAD_STATE_RUNNING);
    TEST_ASSERT_NULL(t->active_wait_blocks);
    return TEST_SUCCESS;
}

struct wait_racer {
    struct thread_wait_header *header;
    atomic_bool *go;
    atomic_uint *wins;
};

static void race_satisfy(void *arg) {
    struct wait_racer *r = arg;
    while (!atomic_load(r->go))
        scheduler_yield();
    if (thread_wait_header_satisfy(r->header,
                                   THREAD_WAKE_REASON_BLOCKING_MANUAL, NULL))
        atomic_inc(r->wins);
}

TEST_DEFINE_UNIT(wait_block, mt_wait_any) {
    struct thread_wait_header headers[2];
    struct thread_wait_object objects[2];
    struct wait_racer racers[2];
    struct thread *wakers[2];
    atomic_bool go = false;
    atomic_uint wins = 0;
    for (size_t i = 0; i < 2; i++) {
        thread_wait_header_init(&headers[i]);
        objects[i] = (struct thread_wait_object){&headers[i], &headers[i]};
        racers[i] = (struct wait_racer){&headers[i], &go, &wins};
        wakers[i] = thread_spawn("wait_racer", race_satisfy, .arg = &racers[i],
                                 .joinable = true);
        enforce(wakers[i]);
    }
    thread_wait_prepare(objects, 2, NULL, THREAD_WAIT_UNINTERRUPTIBLE,
                        THREAD_BLOCK_REASON_MANUAL);
    atomic_store(&go, true);
    uint16_t key = thread_wait_complete().key;
    for (size_t i = 0; i < 2; i++)
        thread_join(wakers[i]);
    TEST_ASSERT_LT(key, 2);
    TEST_ASSERT_EQ(atomic_load(&wins), 1);
    TEST_ASSERT(list_head_empty(&headers[0].waiters));
    TEST_ASSERT(list_head_empty(&headers[1].waiters));
    return TEST_SUCCESS;
}

struct apc_wait_case {
    struct thread_wait_header header;
    atomic_bool armed;
    atomic_bool lost_registration;
    atomic_uint delivered;
    uint64_t epoch;
};

static void satisfy_during_apc(void *arg) {
    struct apc_wait_case *c = arg;
    struct thread *t = thread_get_current();
    enum irql irql = spin_lock_high(&t->wait_lock);
    if (t->active_wait_blocks != t->wait_blocks || t->wait_epoch != c->epoch ||
        t->wait_blocks[0].state != THREAD_WAIT_BLOCK_ACTIVE || t->alert_pending)
        atomic_store(&c->lost_registration, true);
    spin_unlock(&t->wait_lock, irql);
    /* Four APCs must return to the SAME registration, and fifth completes
     * it from inside delivery */
    if (atomic_fetch_add(&c->delivered, 1) == 4)
        thread_wait_header_satisfy(&c->header,
                                   THREAD_WAKE_REASON_BLOCKING_MANUAL, NULL);
}

static void apc_waiter(void *arg) {
    struct apc_wait_case *c = arg;
    thread_wait_prepare_one(&c->header, c, THREAD_WAIT_INTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);
    c->epoch = thread_get_current()->wait_epoch;
    atomic_store(&c->armed, true);
    thread_wait_complete();
}

TEST_DEFINE_UNIT(wait_block, apc_preserves_registration) {
    struct apc_wait_case c = {0};
    thread_wait_header_init(&c.header);
    struct apc apc;
    apc_init(&apc, satisfy_during_apc, &c, NULL);
    struct thread *t =
        thread_spawn("apc_waiter", apc_waiter, .arg = &c, .joinable = true);
    TEST_ASSERT_NONNULL(t);
    while (!atomic_load(&c.armed))
        scheduler_yield();
    for (size_t i = 0; i < 5; i++) {
        enforce(apc_enqueue(t, &apc, APC_TYPE_KERNEL));
        while (atomic_load_acq(&apc.state) != APC_STATE_IDLE)
            scheduler_yield();
    }
    thread_join(t);
    apc_put(&apc);
    TEST_ASSERT(!atomic_load(&c.lost_registration));
    TEST_ASSERT_EQ(atomic_load(&c.delivered), 5);
    TEST_ASSERT(list_head_empty(&c.header.waiters));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(wait_block, uninterruptible_latches_alert) {
    struct thread_wait_header header;
    struct thread *t = thread_get_current();
    thread_wait_header_init(&header);
    thread_wait_prepare_one(&header, &header, THREAD_WAIT_UNINTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);
    thread_alert(t);
    bool linked = !list_head_empty(&header.waiters);
    thread_wait_satisfy(&t->wait_blocks[0], THREAD_WAKE_REASON_SLEEP_MANUAL,
                        NULL);
    struct thread_wait_result result = thread_wait_complete();
    TEST_ASSERT_EQ(result.status, THREAD_WAIT_SATISFIED);
    TEST_ASSERT(linked);
    TEST_ASSERT(t->alert_pending);
    thread_park(); /* Consumes the latched permit without blocking */
    TEST_ASSERT(!t->alert_pending);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(wait_block, stale_timeout_cannot_satisfy_next_wait) {
    struct thread_wait_header header;
    struct thread *t = thread_get_current();
    thread_wait_header_init(&header);
    thread_wait_prepare_one(&header, &header, THREAD_WAIT_UNINTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);
    uint64_t old_epoch = t->wait_epoch;
    thread_wait_cancel();
    thread_wait_prepare_one(&header, &header, THREAD_WAIT_UNINTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);
    bool stale_won = thread_wait_satisfy_epoch(
        &t->wait_blocks[0], old_epoch, THREAD_WAKE_REASON_SLEEP_TIMEOUT, NULL);
    thread_wait_satisfy(&t->wait_blocks[0], THREAD_WAKE_REASON_SLEEP_MANUAL,
                        NULL);
    struct thread_wait_result result = thread_wait_complete();
    TEST_ASSERT(!stale_won);
    TEST_ASSERT_EQ(result.reason, THREAD_WAKE_REASON_SLEEP_MANUAL);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(wait_block, alert_wait_any_and_single_permit) {
    struct thread *t = thread_get_current();
    struct thread_wait_header headers[2];
    struct thread_wait_object objects[2];
    for (size_t i = 0; i < 2; i++) {
        thread_wait_header_init(&headers[i]);
        objects[i] = (struct thread_wait_object){&headers[i], &headers[i]};
    }
    thread_alert(t);
    thread_alert(t);
    thread_wait_prepare(objects, 2, NULL, THREAD_WAIT_INTERRUPTIBLE,
                        THREAD_BLOCK_REASON_MANUAL);
    struct thread_wait_result first = thread_wait_complete();
    TEST_ASSERT_EQ(first.status, THREAD_WAIT_ALERTED);
    TEST_ASSERT_EQ(first.key, THREAD_WAIT_KEY_NONE);
    TEST_ASSERT(list_head_empty(&headers[0].waiters));
    TEST_ASSERT(list_head_empty(&headers[1].waiters));

    thread_wait_prepare(objects, 2, NULL, THREAD_WAIT_INTERRUPTIBLE,
                        THREAD_BLOCK_REASON_MANUAL);
    bool still_waiting = !t->wait_done;
    thread_alert(t);
    struct thread_wait_result second = thread_wait_complete();
    TEST_ASSERT(still_waiting); /* The two earlier alerts coalesced */
    TEST_ASSERT_EQ(second.status, THREAD_WAIT_ALERTED);
    TEST_ASSERT(list_head_empty(&headers[0].waiters));
    TEST_ASSERT(list_head_empty(&headers[1].waiters));
    return TEST_SUCCESS;
}
