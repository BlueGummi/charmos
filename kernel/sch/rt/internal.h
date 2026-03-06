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

void rt_scheduler_acquire_two_locks(struct rt_scheduler *a,
                                    struct rt_scheduler *b, enum irql *oa,
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
