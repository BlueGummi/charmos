#include <mem/slab.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/percpu.h>

#include "internal.h"

static void scheduler_percpu_work_ctor(struct scheduler_periodic_work_percpu *,
                                       cpu_id_t);

SLAB_SIZE_REGISTER_FOR_STRUCT(scheduler_periodic_work);
PERCPU_DECLARE(periodic_percpu, struct scheduler_periodic_work_percpu,
               scheduler_percpu_work_ctor);

static int32_t work_cmp(struct pairing_node *a, struct pairing_node *b) {
    struct scheduler_periodic_work *wa =
        container_of(a, struct scheduler_periodic_work, pnode);
    struct scheduler_periodic_work *wb =
        container_of(b, struct scheduler_periodic_work, pnode);

    if (wa->expected_next < wb->expected_next)
        return -1;

    if (wa->expected_next > wb->expected_next)
        return 1;

    return (uintptr_t) wa < (uintptr_t) wb ? -1 : 1;
}

static void
scheduler_percpu_work_ctor(struct scheduler_periodic_work_percpu *pcpu,
                           cpu_id_t cpu) {
    pcpu->cpu = cpu;
    for (size_t i = 0; i < PERIODIC_WORK_MAX; i++) {
        pairing_heap_init(&pcpu->period_based_works[i], work_cmp);
        pairing_heap_init(&pcpu->time_based_works[i], work_cmp);
        pcpu->limits.max_duration_per_call_ns = TIME_MAX;
        pcpu->limits.max_execs_per_call = SIZE_MAX;
    }
}

static void
linker_object_work_to_work(struct scheduler_periodic_work_linker_object *lobj,
                           struct scheduler_periodic_work *pw) {
    pw->name = lobj->name;
    pw->fn = lobj->fn;
    pw->interval = lobj->interval;
    pw->type = lobj->type;
    pw->prio = lobj->prio;
}

static void
attach_work_to_cpus(struct scheduler_periodic_work_linker_object *spwlo) {
    for (size_t i = 0; i < global.core_count; i++) {
        struct scheduler_periodic_work *w = kzalloc(
            sizeof(struct scheduler_periodic_work), ALLOC_PARAMS_DEFAULT);
        if (!w)
            k_panic("OOM\n");

        pairing_node_init(&w->pnode);
        linker_object_work_to_work(spwlo, w);
        struct scheduler_periodic_work_percpu *pcpu =
            &PERCPU_READ_FOR_CPU(periodic_percpu, i);

        if (w->type == PERIODIC_WORK_TIME_BASED) {
            pairing_heap_insert(&pcpu->time_based_works[w->prio], &w->pnode);
            pcpu->time_based_work_count[w->prio]++;
        } else {
            pairing_heap_insert(&pcpu->period_based_works[w->prio], &w->pnode);
            pcpu->period_based_work_count[w->prio]++;
        }
    }
}

void scheduler_periodic_work_init(void) {
    for (struct scheduler_periodic_work_linker_object *spw =
             __skernel_sched_periodic_work;
         spw < __ekernel_sched_periodic_work; spw++) {
        attach_work_to_cpus(spw);
    }
}

static bool periodic_work_exec(uint64_t current,
                               struct scheduler_periodic_work *pw) {
    if (current >= pw->expected_next) {
        pw->fn();
        pw->executed_times++;
        pw->last_occurrence = current;
        pw->expected_next += pw->interval;

        if (current > pw->expected_next) {
            size_t missed = (current - pw->expected_next) / pw->interval;
            pw->interval_total_loss += missed * pw->interval;
        }

        pw->interval_latency = pw->interval_total_loss / pw->executed_times;

        return true;
    }

    return false;
}

static bool passed_limit(time_t initial_time, size_t executed,
                         struct scheduler_periodic_work_percpu *percpu) {
    return !(time_get_us() * 1000 - initial_time <
                 percpu->limits.max_duration_per_call_ns &&
             executed < percpu->limits.max_execs_per_call);
}

/* The IRQL guarantees that we will NEVER have to do locking on
 * percpu state inside of these functions */
void scheduler_periodic_work_execute(enum scheduler_periodic_work_type type) {
    if (global.current_bootstage < BOOTSTAGE_LATE)
        return;

    kassert(scheduler_preemption_disabled());
    kassert(irql_get() == IRQL_DISPATCH_LEVEL);
    kassert(!scheduler_in_periodic_work());

    struct scheduler_periodic_work_percpu *pcpu = &PERCPU_READ(periodic_percpu);
    pcpu->executing = true;

    bool time_based = type == PERIODIC_WORK_TIME_BASED;
    time_t initial_time = time_get_us() * 1000;
    size_t executed = 0;
    size_t current_period = smp_core_scheduler()->current_period;
    size_t starting_point = time_based ? initial_time : current_period;

    for (size_t i = PERIODIC_WORK_HIGH; i <= PERIODIC_WORK_LOW; i++) {
        struct pairing_heap *heap;

        if (time_based) {
            heap = &pcpu->time_based_works[i];
        } else {
            heap = &pcpu->period_based_works[i];
        }

        size_t size = time_based ? pcpu->time_based_work_count[i]
                                 : pcpu->period_based_work_count[i];

        size_t executed_in_this_heap = 0;

        while (executed_in_this_heap < size) {
            struct pairing_node *pn = pairing_heap_peek(heap);
            if (!pn)
                break;

            struct scheduler_periodic_work *pw =
                container_of(pn, struct scheduler_periodic_work, pnode);

            if (!periodic_work_exec(starting_point, pw))
                break;

            executed_in_this_heap++;
            executed++;

            pairing_heap_pop(heap);
            pairing_heap_insert(heap, &pw->pnode);

            if (passed_limit(initial_time, executed, pcpu))
                break;
        }
    }

    pcpu->executing = false;
}

bool scheduler_in_periodic_work() {
    return PERCPU_READ(periodic_percpu).executing;
}
