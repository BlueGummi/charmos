#include <sch/irql.h>
#include <sch/rt_sched.h>
#include <sync/spinlock.h>

/* Globally, we keep track of RT_SCHEDULER_SLOTS_PER_THREAD of these. */
struct rt_slot {
    size_t slot_index; /* What index in the slots[] of the thread(s)? */
    struct rt_scheduler_static *in_use; /* From whom? */
};

/* Tracking database structure for all of these */
struct rt_slot_db {
    size_t num_slots;
    struct spinlock lock;
    struct rt_slot *slots;
};

void rt_scheduler_static_destroy_work_enqueue(struct rt_scheduler_static *rts);
enum rt_scheduler_error
rt_slots_init_for_scheduler(struct rt_scheduler_static *rts);

static inline void rt_scheduler_acquire_two_locks(struct rt_scheduler *a,
                                                  struct rt_scheduler *b,
                                                  enum irql *oa,
                                                  enum irql *ob) {
    if (a == b) {
        *oa = spin_lock_irq_disable(&a->lock);
        return;
    }

    if (a < b) {
        *oa = spin_lock_irq_disable(&a->lock);
        *ob = spin_lock_irq_disable(&b->lock);
    } else {
        *ob = spin_lock_irq_disable(&b->lock);
        *oa = spin_lock_irq_disable(&a->lock);
    }
}

static inline enum rt_scheduler_static_state
rt_scheduler_static_get_state(struct rt_scheduler_static *rts) {
    return atomic_load_explicit(&rts->state, memory_order_acquire);
}

REFCOUNT_GENERATE_GET_FOR_STRUCT_WITH_FAILURE_COND(
    rt_scheduler_static, refcount, state, == RT_SCHEDULER_STATE_DESTROYING);

static inline void rt_scheduler_static_put(struct rt_scheduler_static *rts) {
    if (refcount_dec_and_test(&rts->refcount)) {
        kassert(rt_scheduler_static_get_state(rts) ==
                RT_SCHEDULER_STATE_DESTROYING);
        rt_scheduler_static_destroy_work_enqueue(rts);
    }
}
