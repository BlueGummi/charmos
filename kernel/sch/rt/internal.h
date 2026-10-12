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
void rt_slots_dealloc_for_scheduler(struct rt_scheduler_static *rts);
size_t rt_slot_get_num_available(void);

static inline void rt_scheduler_acquire_two_mappings(
    struct rt_scheduler_mapping *a, struct rt_scheduler_mapping *b,
    enum irql *out_a, enum irql *out_b) TSA_NO_ANALYSIS {
    if (a < b) {
        *out_a = spin_lock_high(&a->lock);
        *out_b = spin_lock_high(&b->lock);
    } else if (b < a) {
        *out_b = spin_lock_high(&b->lock);
        *out_a = spin_lock_high(&a->lock);
    } else {
        panic("Trying to acquire two locks on the same mapping");
    }
}

static inline void rt_scheduler_release_two_mappings(
    struct rt_scheduler_mapping *a, struct rt_scheduler_mapping *b,
    enum irql out_a, enum irql out_b) TSA_NO_ANALYSIS {
    if (a < b) {
        spin_unlock(&b->lock, out_b);
        spin_unlock(&a->lock, out_a);
    } else if (b < a) {
        spin_unlock(&a->lock, out_a);
        spin_unlock(&b->lock, out_b);
    } else {
        panic("Trying to release two locks on the same mapping");
    }
}

static inline void
rt_scheduler_acquire_two_locks(struct rt_scheduler *a, struct rt_scheduler *b,
                               enum irql *oa, enum irql *ob) TSA_NO_ANALYSIS {
    if (a == b) {
        *oa = spin_lock_high(&a->lock);
        return;
    }

    if (a < b) {
        *oa = spin_lock_high(&a->lock);
        *ob = spin_lock_high(&b->lock);
    } else {
        *ob = spin_lock_high(&b->lock);
        *oa = spin_lock_high(&a->lock);
    }
}

static inline void
rt_scheduler_release_two_locks(struct rt_scheduler *a, struct rt_scheduler *b,
                               enum irql oa, enum irql ob) TSA_NO_ANALYSIS {
    if (a == b) {
        spin_unlock(&a->lock, oa);
        return;
    }

    if (a < b) {
        spin_unlock(&b->lock, ob);
        spin_unlock(&a->lock, oa);
    } else {
        spin_unlock(&a->lock, oa);
        spin_unlock(&b->lock, ob);
    }
}

static inline enum rt_scheduler_static_state
rt_scheduler_static_get_state(struct rt_scheduler_static *rts) {
    return atomic_load_acq(&rts->state);
}

static inline void
rt_scheduler_static_set_state(struct rt_scheduler_static *rts,
                              enum rt_scheduler_static_state new) {
    atomic_store_release(&rts->state, new);
}

REFCOUNT_GENERATE_GET_FOR_STRUCT_WITH_FAILURE_COND(
    rt_scheduler_static, refcount, state, == RT_SCHEDULER_STATIC_DESTROYING);

static inline void rt_scheduler_static_put(struct rt_scheduler_static *rts) {
    if (refcount_dec_and_test(&rts->refcount)) {
        enforce(rt_scheduler_static_get_state(rts) ==
                RT_SCHEDULER_STATIC_DESTROYING);
        rt_scheduler_static_destroy_work_enqueue(rts);
    }
}
