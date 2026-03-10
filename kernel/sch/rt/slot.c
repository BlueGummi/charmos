#include <mem/alloc.h>
#include <sch/irql.h>
#include <sch/rt_sched.h>
#include <sync/spinlock.h>

#include "internal.h"

static struct rt_slot_db slot_db;

struct rt_slot *rt_slot_allocate(struct rt_scheduler_static *for_whom) {
    enum irql irql = spin_lock_irq_disable(&slot_db.lock);

    struct rt_slot *got = NULL;
    for (size_t i = 0; i < slot_db.num_slots; i++) {
        struct rt_slot *try = &slot_db.slots[i];
        if (!try->in_use) {
            got = try;
            try->in_use = for_whom;
            break;
        }
    }

    spin_unlock(&slot_db.lock, irql);
    return got;
}

void rt_slot_free(size_t slot) {
    enum irql irql = spin_lock_irq_disable(&slot_db.lock);

    slot_db.slots[slot].in_use = NULL;

    spin_unlock(&slot_db.lock, irql);
}

void rt_slot_init(size_t num_slots) {
    slot_db.num_slots = num_slots;
    spinlock_init(&slot_db.lock);
    slot_db.slots =
        kzalloc(sizeof(struct rt_slot) * num_slots, ALLOC_PARAMS_DEFAULT);

    if (!slot_db.slots)
        panic("OOM\n");

    for (size_t i = 0; i < num_slots; i++)
        slot_db.slots[i].slot_index = i;
}

static void free_all_slots(struct rt_scheduler_static *rts) {
    for (size_t i = 0; i < rts->num_slot_requests; i++) {
        struct rt_slot_request *rq = &rts->slot_requests[i];
        if (rq->mapped_to != -1)
            rt_slot_free(rq->mapped_to);

        rq->mapped_to = -1;
    }
}

enum rt_scheduler_error
rt_slots_init_for_scheduler(struct rt_scheduler_static *rts) {
    rt_sched_trace("Initializing slots for %s (%p)", rts->name, rts);
    size_t need = rts->num_slot_requests;
    for (size_t i = 0; i < need; i++) {
        struct rt_slot_request *rq = &rts->slot_requests[i];
        struct rt_slot *got = rt_slot_allocate(rts);

        if (got) {
            rq->mapped_to = got->slot_index;
        } else if (rq->prio == RT_SLOT_REQUIRED) {
            free_all_slots(rts);
            return RT_SCHEDULER_ERR_OOR;
        }
    }

    return RT_SCHEDULER_ERR_OK;
}
