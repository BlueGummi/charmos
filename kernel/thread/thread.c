#include <bootstage.h>
#include <log.h>
#include <mem/address_range.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vas.h>
#include <mem/vmm.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/domain.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sync/rcu.h>
#include <sync/turnstile.h>
#include <thread/apc.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/tid.h>
#include <thread/workqueue.h>
#include <time/timer.h>

#include "sch/internal.h"

#ifdef DEBUG_LOCK_CHK

#include "sync/lock_chk/internal.h"

static void thread_lock_chk_init(struct thread *thread) {
    lock_chk_thread_init(thread);
}

static void thread_lock_chk_exit(struct thread *thread) {
    lock_chk_thread_exit(thread);
}

#else

static void thread_lock_chk_init(struct thread *thread) {
    cc_ignore(thread);
}

static void thread_lock_chk_exit(struct thread *thread) {
    cc_ignore(thread);
}

#endif

SLAB_SIZE_REGISTER_FOR_STRUCT(thread, /*alignment*/ 32);

ADDRESS_RANGE_DEFINE(thread_stacks, .name = "thread stacks",
                     .base = THREAD_STACKS_HEAP_START,
                     .size = THREAD_STACKS_HEAP_END - THREAD_STACKS_HEAP_START,
                     .flags = ADDRESS_RANGE_STATIC);

/* lol */
static struct tid_space *global_tid_space = NULL;
static struct vas *stacks_space = NULL;

void thread_init_thread_ids(void) {
    stacks_space = vas_create(THREAD_STACKS_HEAP_START, THREAD_STACKS_HEAP_END);
    global_tid_space = tid_space_init(UINT64_MAX);
    locked_list_init(&global.thread_list, LOCKED_LIST_INIT_IRQ_DISABLE);
}

APC_EVENT_CREATE(thread_exit_apc_event, "THREAD_EXIT");

void thread_exit(void) {
    thread_exit_with_status(0);
}

void thread_exit_with_status(int status) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    struct thread *self = thread_get_current();
    thread_lock_chk_exit(self);

    /* Can't be in a critical section  when we exit */
    enforce(self->rcu_nesting == 0, "thread exited inside an RCU read section");

    /* Public status and the ZOMBIE state under join_lock...
     *
     * Joiners either see ZOMBIE here are don't block, or are already
     * parked on join_cv and gets the broadcast, no third outcome */
    enum irql jirql = spin_lock(&self->join_lock);
    self->exit_status = status;
    thread_set_state(self, THREAD_STATE_ZOMBIE);
    thread_or_flags(self, THREAD_FLAG_DYING);
    spin_unlock(&self->join_lock, jirql);

    apc_rundown_thread(self);

    /* Resumed joiners can't free because our ref stays alive
     * until dropped by the next thread */
    condvar_broadcast(&self->join_cv);

    climb_thread_remove(self);
    locked_list_del(&global.thread_list, &self->thread_list);
    atomic_dec(&global.thread_count);

    irql_lower(irql);

    scheduler_yield();
    ci_unreachable();
}

void thread_entry_wrapper(void) TSA_NO_ANALYSIS {
    /* TODO: We might want to consider refactoring
     * the switch_in so as to not gradually bloat up
     * both places where the "gopher pops out of the ground",
     * i.e. a thread entering */
    scheduler_yield_nesting_reset(thread_get_current());

    if (atomic_load_rlx(&thread_get_current()->state) !=
        THREAD_STATE_IDLE_THREAD)
        atomic_inc(&global.thread_count);

    void (*entry)(void *);
    asm volatile("mov %%r12, %0" : "=r"(entry));

    void *arg;
    asm volatile("mov %%r13, %0" : "=r"(arg));

    scheduler_switch_in();

    enforce(bootstage_get() < BOOTSTAGE_LATE ||
                irql_get() == IRQL_DISPATCH_LEVEL,
            "entered thread at %s, want %s", irql_to_str(irql_get()),
            irql_to_str(IRQL_DISPATCH_LEVEL));

    scheduler_mark_self_in_resched(false);

    irql_lower(IRQL_PASSIVE_LEVEL);
    enforce(entry);
    entry(arg);
    thread_exit();
}

void *thread_allocate_stack(size_t pages) {
    size_t needed = (pages + 1) * PAGE_SIZE;
    vaddr_t virt_base = vas_alloc(stacks_space, needed, PAGE_SIZE);
    if (!virt_base)
        return NULL;

    /* Leave the first page unmapped, protector page */
    virt_base += PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        vaddr_t virt = virt_base + (i * PAGE_SIZE);
        paddr_t phys = enforce(pmm_alloc_page());
        vmm_map_page(virt, phys,
                     .page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_XD);
    }
    return (void *) virt_base;
}

void thread_free_stack(struct thread *thread) {
    vaddr_t stack_real_virt = (vaddr_t) thread->stack - PAGE_SIZE;
    size_t pages = thread->stack_size / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        vaddr_t virt = (vaddr_t) thread->stack + i * PAGE_SIZE;
        paddr_t phys = vmm_get_phys(virt);
        enforce(phys != (paddr_t) -1);
        vmm_unmap_page(virt);
        pmm_free_page(phys);
    }
    vas_free(stacks_space, stack_real_virt, (pages + 1) * PAGE_SIZE);
}

static void thread_init_event_reasons(
    struct thread_event_reason reasons[THREAD_EVENT_RINGBUFFER_CAPACITY]) {
    for (size_t i = 0; i < THREAD_EVENT_RINGBUFFER_CAPACITY; i++) {
        reasons[i].associated_reason.reason = THREAD_EVENT_REASON_NONE;
        reasons[i].associated_reason.cycle = 0;
        reasons[i].reason = THREAD_EVENT_REASON_NONE;
        reasons[i].cycle = 0;
        reasons[i].timestamp = 0;
    }
}

static void thread_init_activity_data(struct thread *thread) {
    struct thread_activity_data *data = thread->activity_data;
    data->block_reasons_head = 0;
    data->sleep_reasons_head = 0;
    data->wake_reasons_head = 0;
    thread_init_event_reasons(thread->activity_data->block_reasons);
    thread_init_event_reasons(thread->activity_data->wake_reasons);
    thread_init_event_reasons(thread->activity_data->sleep_reasons);
}

static struct thread *thread_init(struct thread *thread,
                                  thread_entry_fn_t entry,
                                  struct thread_create_params *params) {
    thread_init_activity_data(thread);
    thread_lock_chk_init(thread);
    memset(thread->activity_stats, 0, sizeof(struct thread_activity_stats));

    uint64_t stack_top =
        (uint64_t) params->stack_internal + params->stack_pages * PAGE_SIZE;
    thread->allowed_cpus = params->allowed_cpus;
    thread->private = params->private;

    thread->entry = entry;
    thread->creation_time_ms = time_get_ms();
    thread->stack_size = params->stack_pages * PAGE_SIZE;
    thread->regs.rsp = stack_top;
    atomic_init(&thread->migrate_to, -1);
    thread->base_prio_class = THREAD_PRIO_CLASS_TIMESHARE;
    thread->niceness = 0;
    thread->perceived_prio_class = THREAD_PRIO_CLASS_TIMESHARE;
    atomic_init(&thread->state, THREAD_STATE_READY);
    thread->regs.r12 = (uint64_t) entry;
    thread->regs.r13 = (uint64_t) params->arg;
    thread->regs.rip = (uint64_t) thread_entry_wrapper;
    thread->stack = (void *) params->stack_internal;
    atomic_init(&thread->flags, params->flags);
    thread->curr_core = -1;
    thread->rcu_nesting = 0;
    thread->rcu_read_seq = 0;
    thread->rcu_leaf = NULL;
    thread->rcu_blocked_seq = 0;
    thread->id = tid_alloc(global_tid_space);
    refcount_init(&thread->refcount, 1);
    thread->timeslice_length_raw_ms = THREAD_DEFAULT_TIMESLICE;
    atomic_init(&thread->wait_type, THREAD_WAIT_NONE);
    thread->activity_class = THREAD_ACTIVITY_CLASS_UNKNOWN;
    thread->exit_status = 0;
    spinlock_init(&thread->lock);
    thread_wait_init(thread);
    spinlock_init(&thread->join_lock);
    spinlock_init(&thread->pi_lock);

    /* join_lock/join_cv are only ever touched from thread context, and
     * thread_join_timeout() has to allocate a timer while holding the
     * lock, which it could not do at IRQL_HIGH_LEVEL */
    condvar_init(&thread->join_cv);
    pairing_node_init(&thread->wq_pairing_node);

    turnstile_init(thread->turnstile);

    thread_update_effective_priority(thread);

    climb_thread_init(thread);
    list_head_init(&thread->io_wait_tokens);
    list_node_init(&thread->thread_list);

    for (size_t i = 0; i < APC_TYPE_COUNT; i++)
        apc_queue_init(&thread->apc_head[i]);

    apc_queue_init(&thread->event_apcs);
    apc_queue_init(&thread->to_exec_event_apcs);

    list_node_init(&thread->rq_list_node);
    list_node_init(&thread->wq_list_node);
    list_node_init(&thread->rcu_list_node);
    list_node_init(&thread->reaper_list);
    rbt_init_node(&thread->rq_tree_node);
    rbt_init_node(&thread->wq_tree_node);
    crash_perthread_init(thread);
    locked_list_add(&global.thread_list, &thread->thread_list);

    return thread;
}

struct thread *thread_create_full(char *name, thread_entry_fn_t entry,
                                  struct thread_create_params *params,
                                  va_list args) {
    enforce(name);
    struct thread *new_thread = vnew(new_thread, ALLOC_ZERO);
    if (cc_unlikely(!new_thread))
        goto err;

    void *stack = thread_allocate_stack(params->stack_pages);
    if (cc_unlikely(!stack))
        goto err;

    params->stack_internal = stack;
    new_thread->activity_data = vnew(new_thread->activity_data, ALLOC_ZERO);
    if (cc_unlikely(!new_thread->activity_data))
        goto err;

    new_thread->turnstile = turnstile_create();
    if (cc_unlikely(!new_thread->turnstile))
        goto err;

    new_thread->activity_stats = vnew(new_thread->activity_stats, ALLOC_ZERO);
    if (cc_unlikely(!new_thread->activity_stats))
        goto err;

    cpu_mask_init(&new_thread->allowed_cpus, global.core_count);

    cpu_mask_set_all(&new_thread->allowed_cpus);

    va_list args_copy;
    va_copy(args_copy, args);
    size_t needed = vsnprintf(NULL, 0, name, args_copy) + 1;
    va_end(args_copy);

    new_thread->name = valloc(needed, ALLOC_ZERO);
    if (!new_thread->name)
        goto err;

    struct log_site_options opts = {
        .name = new_thread->name,
        .dump_opts = LOG_DUMP_DEFAULT,
        .capacity = 16,
        .flags = LOG_SITE_DEFAULT,
        .enabled_mask = LOG_SITE_ALL,
    };

    new_thread->log_site = log_site_create(opts);
    if (!new_thread->log_site)
        goto err;

    new_thread->log_handle = LOG_HANDLE_DEFAULT;
    va_copy(args_copy, args);
    vsnprintf(new_thread->name, needed, name, args_copy);
    va_end(args_copy);

    return thread_init(new_thread, entry, params);

err:
    if (!new_thread)
        return NULL;

    vfree(new_thread->turnstile);
    vfree(new_thread->name);
    vfree(new_thread->activity_data);
    vfree(new_thread->activity_stats);
    thread_free_stack(new_thread);
    tid_free(global_tid_space, new_thread->id);
    vfree(new_thread);

    return NULL;
}

/* TODO: This funny business is not needed with RCU */
void thread_set_joinable(struct thread *t) {
    enforce(!(thread_get_flags(t) & THREAD_FLAG_JOINABLE));
    enforce(refcount_inc(&t->refcount));
    thread_set_flag(t, THREAD_FLAG_JOINABLE);
}

struct thread *thread_create_internal(thread_entry_fn_t entry,
                                      struct thread_create_params params,
                                      char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    struct thread *ret = thread_create_full(fmt, entry, &params, args);
    va_end(args);
    if (ret && params.joinable)
        thread_set_joinable(ret);

    return ret;
}

struct thread *thread_spawn_internal(thread_entry_fn_t entry,
                                     struct thread_create_params params,
                                     char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    struct thread *t = thread_create_full(fmt, entry, &params, args);
    va_end(args);

    if (t && params.joinable)
        thread_set_joinable(t);

    if (t && params.on_cpu == CPU_ID_MAX) {
        thread_enqueue(t);
    } else if (t) {
        thread_enqueue_on_core(t, params.on_cpu);
    }

    return t;
}

void thread_free(struct thread *t) {
    tid_free(global_tid_space, t->id);
    vfree(t->activity_data);
    vfree(t->activity_stats);
    vfree(t->name);
    vfree(t->turnstile);
    apc_rundown_thread(t);
    thread_free_stack(t);
    log_site_putref(t->log_site);
    vfree(t);
}

static void thread_reap_rcu(struct rcu_cb *cb) {
    reaper_enqueue(container_of(cb, struct thread, free_rcu));
}

void thread_putref(struct thread *t) {
    if (!refcount_dec_and_test(&t->refcount))
        return;

    if (thread_get_state(t) != THREAD_STATE_ZOMBIE)
        panic("final ref dropped while thread not zombie");

    rcu_defer(&t->free_rcu, thread_reap_rcu);
}

void thread_queue_init(struct thread_queue *q) {
    list_head_init(&q->list);
    spinlock_init(&q->lock);
}

void thread_queue_push_back(struct thread_queue *q, struct thread *t) {
    enum irql irql = spin_lock_high(&q->lock);
    list_head_push_back(&q->list, &t->wq_list_node);
    spin_unlock(&q->lock, irql);
}

bool thread_queue_remove(struct thread_queue *q, struct thread *t) {
    enum irql irql = spin_lock_high(&q->lock);
    struct list_node *pos;

    list_for_each(pos, &q->list) {
        struct thread *thread = thread_from_wq_list_node(pos);
        if (thread == t) {
            list_node_delete_init(&t->wq_list_node);
            spin_unlock(&q->lock, irql);
            return true;
        }
    }

    spin_unlock(&q->lock, irql);
    return false;
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    enum irql irql = spin_lock_high(&q->lock);
    struct list_node *lhead = list_head_pop_front_init(&q->list);
    spin_unlock(&q->lock, irql);
    if (!lhead)
        return NULL;

    return thread_from_wq_list_node(lhead);
}

static void wake_thread_timer_cb(struct timer *timer) {
    struct thread_wait_header *wait = timer->data;
    thread_wait_header_satisfy(wait, THREAD_WAKE_REASON_SLEEP_TIMEOUT, NULL);
}

void thread_sleep_for_us(uint64_t us) {
    if (us == 0) {
        scheduler_yield();
        return;
    }

    struct thread_wait_header wait;
    thread_wait_header_init(&wait);
    struct timer sleep_timer;
    timer_init(&sleep_timer, wake_thread_timer_cb, &wait);

    thread_wait_prepare_to_sleep(&wait, &sleep_timer,
                                 THREAD_WAIT_UNINTERRUPTIBLE);
    timer_modify(&sleep_timer, timer_delta_us(us));

    thread_wait_complete();
    timer_delete_sync(&sleep_timer);
}

void thread_sleep_for_ms(uint64_t ms) {
    thread_sleep_for_us(MS_TO_US(ms));
}

enum irql thread_lock_scheduler(struct thread *t, struct scheduler **out_sched)
    TSA_ACQUIRES_SPIN(&(*out_sched)->lock) TSA_NO_ANALYSIS {
    do {
        size_t gen1 = thread_get_migration_generation(t);
        struct scheduler *sched = thread_get_scheduler_unsafe(t);
        enum irql sirql = spin_lock_high(&sched->lock);
        size_t gen2 = thread_get_migration_generation(t);

        if (gen1 == gen2 && !(gen1 & 1)) {
            *out_sched = sched;
            return sirql;
        }

        spin_unlock(&sched->lock, sirql);
    } while (true);

    panic("unreachable");
}

void thread_lock_two_runqueues(struct thread *a, struct thread *b,
                               struct scheduler **out_rq_a,
                               struct scheduler **out_rq_b, enum irql *irq_a,
                               enum irql *irq_b) TSA_NO_ANALYSIS {
    size_t gen_a1 = 0;
    size_t gen_a2 = 0;
    size_t gen_b1 = 0;
    size_t gen_b2 = 0;

retry:
    gen_a1 = thread_get_migration_generation(a);
    gen_b1 = thread_get_migration_generation(b);

    if ((gen_a1 | gen_b1) & 1)
        goto retry;

    struct scheduler *rq_a = thread_get_scheduler_unsafe(a);
    struct scheduler *rq_b = thread_get_scheduler_unsafe(b);

    gen_a2 = thread_get_migration_generation(a);
    gen_b2 = thread_get_migration_generation(b);

    /* Snapshot must be stable */
    if (gen_a1 != gen_a2 || gen_b1 != gen_b2)
        goto retry;

    struct scheduler *first, *second;

    if (rq_a == rq_b) {
        first = rq_a;
        second = NULL;
    } else if (rq_a < rq_b) {
        first = rq_a;
        second = rq_b;
    } else {
        first = rq_b;
        second = rq_a;
    }

    enum irql first_irql = spin_lock_high(&first->lock);
    enum irql second_irql = first_irql;

    if (second)
        second_irql = spin_lock_high(&second->lock);

    if (thread_get_migration_generation(a) != gen_a1 ||
        thread_get_migration_generation(b) != gen_b1 ||
        thread_get_scheduler_unsafe(a) != rq_a ||
        thread_get_scheduler_unsafe(b) != rq_b) {

        if (second)
            spin_unlock(&second->lock, second_irql);

        spin_unlock(&first->lock, first_irql);

        goto retry;
    }

    *out_rq_a = rq_a;
    *out_rq_b = rq_b;
    *irq_a = rq_a == first ? first_irql : second_irql;
    *irq_b = rq_b == first ? first_irql : second_irql;
}

void thread_lock_thread_and_rq(struct thread *t, struct scheduler *other_rq,
                               struct scheduler **out_thread_rq,
                               enum irql *irq_first,
                               enum irql *irq_second) TSA_NO_ANALYSIS {
    size_t gen1, gen2;

retry:
    gen1 = thread_get_migration_generation(t);

    if (gen1 & 1)
        goto retry;

    struct scheduler *thread_rq = thread_get_scheduler_unsafe(t);

    gen2 = thread_get_migration_generation(t);

    if (gen1 != gen2)
        goto retry;

    struct scheduler *first, *second;

    if (thread_rq == other_rq) {
        first = thread_rq;
        second = NULL;
    } else if (thread_rq < other_rq) {
        first = thread_rq;
        second = other_rq;
    } else {
        first = other_rq;
        second = thread_rq;
    }

    *irq_first = spin_lock_high(&first->lock);

    if (second)
        *irq_second = spin_lock_high(&second->lock);

    if (thread_get_migration_generation(t) != gen1 ||
        thread_get_scheduler_unsafe(t) != thread_rq) {

        if (second)
            spin_unlock(&second->lock, *irq_second);

        spin_unlock(&first->lock, *irq_first);
        goto retry;
    }

    *out_thread_rq = thread_rq;
}

void thread_unlock_thread_and_rq(struct scheduler *thread_rq,
                                 struct scheduler *other_rq,
                                 enum irql irq_first,
                                 enum irql irq_second) TSA_NO_ANALYSIS {
    struct scheduler *first, *second;

    if (thread_rq == other_rq) {
        first = thread_rq;
        second = NULL;
    } else if (thread_rq < other_rq) {
        first = thread_rq;
        second = other_rq;
    } else {
        first = other_rq;
        second = thread_rq;
    }

    if (second)
        spin_unlock(&second->lock, irq_second);

    spin_unlock(&first->lock, irq_first);
}

void thread_lock_two_pi_locks(struct thread *a, struct thread *b,
                              enum irql *irq_a,
                              enum irql *irq_b) TSA_NO_ANALYSIS {
    enforce(a && b && a != b);
    if (a < b) {
        *irq_a = spin_lock_high_subclass(&a->pi_lock, LOCK_SUBCLASS_LOWER);
        *irq_b = spin_lock_high_subclass(&b->pi_lock, LOCK_SUBCLASS_HIGHER);
    } else {
        *irq_b = spin_lock_high_subclass(&b->pi_lock, LOCK_SUBCLASS_LOWER);
        *irq_a = spin_lock_high_subclass(&a->pi_lock, LOCK_SUBCLASS_HIGHER);
    }
}

void thread_unlock_two_pi_locks(struct thread *a, struct thread *b,
                                enum irql *irq_a,
                                enum irql *irq_b) TSA_NO_ANALYSIS {
    enforce(a && b && a != b);
    if (a < b) {
        spin_unlock(&b->pi_lock, *irq_b);
        spin_unlock(&a->pi_lock, *irq_a);
    } else {
        spin_unlock(&a->pi_lock, *irq_a);
        spin_unlock(&b->pi_lock, *irq_b);
    }
}

/* TODO: This is surprisngly tricky to implement, and the primary reason why
 * is because of invocations during boundaries. */
bool thread_in_context(void) {
    if (global.current_bootstage < BOOTSTAGE_LATE)
        return false;

    if (irq_in_interrupt() || irq_in_nmi())
        return false;

    struct thread *self = thread_get_current();
    if (thread_test_flag(self, THREAD_FLAG_EXECUTING_APC))
        return false;

    return true;
}
