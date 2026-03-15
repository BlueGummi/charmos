#include <math/fixed.h>
#include <mem/alloc.h>
#include <mem/numa.h>
#include <sch/rt_sched.h>
#include <sch/sched.h>
#include <smp/topology.h>
#include <sync/spinlock.h>

#include "internal.h"
#include "sch/internal.h"

struct rt_global rt_global = {0};
struct workqueue *rt_wq = NULL;
static void destroy_work(void *a, void *b);

void rt_scheduler_static_destroy_work_enqueue(struct rt_scheduler_static *rts) {
    struct work *this_work = &rts->teardown_work;
    workqueue_enqueue(rt_wq, this_work);
}

void rt_scheduler_static_work_init(struct rt_scheduler_static *rts) {
    struct work *this_work = &rts->teardown_work;
    work_init(this_work, destroy_work, WORK_ARGS(rts, NULL));
}

static struct rt_scheduler_mapping *
create_mapping(struct rt_scheduler_static *rts, rt_domain_id_t id) {
    struct rt_scheduler_mapping *ret =
        kzalloc(sizeof(struct rt_scheduler_mapping), ALLOC_PARAMS_DEFAULT);
    if (!ret)
        return NULL;

    if (!cpu_mask_init(&ret->members, global.core_count)) {
        kfree(ret, FREE_PARAMS_DEFAULT);
        return NULL;
    }

    if (!cpu_mask_init(&ret->active, global.core_count)) {
        cpu_mask_deinit(&ret->members);
        kfree(ret, FREE_PARAMS_DEFAULT);
        return NULL;
    }

    ret->id = id;
    ret->static_bptr = rts;
    rbt_init_node(&ret->tree_node);
    rbt_insert(&rts->mappings_internal, &ret->tree_node);
    spinlock_init(&ret->lock);

    return NULL;
}

/* Fails only on OOM */
static bool add_to_rbt_or_set_cpu_mask(struct rt_scheduler_static *rts,
                                       rt_domain_id_t id, struct core *core) {
    struct rbt_node *node = rbt_search(&rts->mappings_internal, id);
    struct rt_scheduler_mapping *mapping;
    if (node) {
        mapping = container_of(node, struct rt_scheduler_mapping, tree_node);
    } else {
        mapping = create_mapping(rts, id);
    }

    if (!mapping)
        return false;

    cpu_mask_set(&mapping->members, core->id);
    return true;
}

static void destroy_rt_mappings(struct rt_scheduler_static *rts) {
    struct rbt_node *iter, *tmp;
    rbt_for_each_safe(iter, tmp, &rts->mappings_internal) {
        rbt_delete(&rts->mappings_internal, iter);

        struct rt_scheduler_mapping *m =
            container_of(iter, struct rt_scheduler_mapping, tree_node);

        kfree(m, FREE_PARAMS_DEFAULT);
    }
}

/* Lock the mapping for this CPU, and return it. */
struct rt_scheduler_mapping *rt_lookup_mapping(struct rt_scheduler_static *rts,
                                               struct core *c) {
    rt_domain_id_t id = rts->ops.domain_id_for_cpu(c);
    struct rbt_node *found = rbt_search(&rts->mappings_internal, id);
    if (!found)
        panic("CPU %zu does not have a mapping in this scheduler", c->id);

    struct rt_scheduler_mapping *mapping =
        container_of(found, struct rt_scheduler_mapping, tree_node);

    /* Must be set */
    kassert(cpu_mask_test(&mapping->members, c->id));

    return mapping;
}

/* We build the mapping ONCE at the very start, and then it becomes RO */
enum rt_scheduler_error rt_build_mapping(struct rt_scheduler_static *rts) {
    struct rt_scheduler_ops *ops = &rts->ops;
    struct core *iter;

    kassert(rts->mappings_internal.root == NULL);

    /* Let's go through every CPU on the system and then make a node
     * for all of them, or add them to an existing node's bitmap */
    for_each_cpu_struct(iter) {
        rt_domain_id_t id = ops->domain_id_for_cpu(iter);
        if (!add_to_rbt_or_set_cpu_mask(rts, id, iter)) {
            destroy_rt_mappings(rts);
            return RT_SCHEDULER_ERR_OOM;
        }
    }

    return RT_SCHEDULER_ERR_OK;
}

static size_t rt_mapping_get_data(struct rbt_node *n) {
    return container_of(n, struct rt_scheduler_mapping, tree_node)->id;
}

static int32_t rt_mapping_cmp(const struct rbt_node *a,
                              const struct rbt_node *b) {
    int32_t l = rt_mapping_get_data((void *) a);
    int32_t r = rt_mapping_get_data((void *) b);
    return l - r;
}

static void reset_summary(struct rt_thread_summary *sum) {
    sum->status = RT_SCHEDULER_STATUS_OK;
    sum->weight = 0;
    sum->urgency = FX(0.0);
}

static void reset_shed_request(struct rt_thread_shed_request *rtsr) {
    rtsr->urgency = FX(0.0);
    rtsr->threads_available = 0;
    kassert(list_empty(&rtsr->threads));
    INIT_LIST_HEAD(&rtsr->threads);
    rtsr->on = false;
}

static void reset_scheduler(struct rt_scheduler *rts) {
    kassert(rts->thread_count == 0);
    rts->mapping_source = NULL;
    reset_summary(&rts->summary);
    reset_shed_request(&rts->shed_request);
}

enum rt_scheduler_error
rt_load_scheduler_static(struct rt_scheduler_static *rts) {
    enum rt_scheduler_error err = RT_SCHEDULER_ERR_INVALID;
    enum irql outer = spin_lock(&rt_global.static_list.lock);
    enum irql irql = spin_lock(&rts->state_change_lock);

    if (rt_scheduler_static_get_state(rts) != RT_SCHEDULER_STATIC_UNLOADED)
        goto done;

    rbt_init(&rts->mappings_internal, rt_mapping_get_data, rt_mapping_cmp);
    refcount_init(&rts->refcount, 1);
    INIT_LIST_HEAD(&rts->list_internal);
    list_add_tail(&rts->list_internal, &rt_global.static_list.list);
    rt_global.static_list.num_elems++;

    if (!(rts->active_mask_internal = cpu_mask_create())) {
        err = RT_SCHEDULER_ERR_OOM;
        goto done;
    }

    if (!cpu_mask_init(rts->active_mask_internal, global.core_count)) {
        err = RT_SCHEDULER_ERR_OOM;
        goto done;
    }

    if ((err = rt_slots_init_for_scheduler(rts)) != RT_SCHEDULER_ERR_OK)
        goto done;

    if ((err = rt_build_mapping(rts)) != RT_SCHEDULER_ERR_OK)
        goto done;

    rt_scheduler_static_work_init(rts);
    err = rts->ops.on_load(rts);

done:
    if (err == RT_SCHEDULER_ERR_OK)
        rt_scheduler_static_set_state(rts, RT_SCHEDULER_STATIC_LOADED);

    spin_unlock(&rts->state_change_lock, irql);
    spin_unlock(&rt_global.static_list.lock, outer);
    return err;
}

enum rt_scheduler_error
rt_unload_scheduler_static(struct rt_scheduler_static *rts) {
    /* In here, we want to validate that we are looking at a loaded
     * scheduler_static, and if we are, we can safely drop the initial
     * ref. Otherwise, return the error (unloading an unloaded scheduler) */
    enum rt_scheduler_error err = RT_SCHEDULER_ERR_INVALID;
    enum irql irql = spin_lock(&rts->state_change_lock);

    if (rts->state != RT_SCHEDULER_STATIC_LOADED)
        goto done;

    rt_scheduler_static_set_state(rts, RT_SCHEDULER_STATIC_DESTROYING);
    rt_scheduler_static_put(rts);
    err = RT_SCHEDULER_ERR_OK;

done:

    spin_unlock(&rts->state_change_lock, irql);
    return err;
}

static void rt_scheduler_destroy_internal(struct rt_scheduler_static *rts) {
    /* Assert that the refs are gone, lock, unload, teardown */
    enum irql outer = spin_lock(&rt_global.static_list.lock);
    enum irql irql = spin_lock(&rts->state_change_lock);
    kassert(refcount_read(&rts->refcount) == 0);
    rt_scheduler_static_set_state(rts, RT_SCHEDULER_STATIC_UNLOADED);

    rts->ops.on_unload(rts);

    list_del_init(&rts->list_internal);
    rt_global.static_list.num_elems--;
    rt_slots_dealloc_for_scheduler(rts);
    cpu_mask_deinit(rts->active_mask_internal);
    cpu_mask_free(rts->active_mask_internal);
    rts->active_mask_internal = NULL;
    destroy_rt_mappings(rts);

    spin_unlock(&rts->state_change_lock, irql);
    spin_unlock(&rt_global.static_list.lock, outer);
}

static void destroy_work(void *a, void *b) {
    (void) b;
    rt_sched_trace("Destroying realtime scheduler %p", a);
    struct rt_scheduler_static *rts = a;
    rt_scheduler_destroy_internal(rts);
}

/* Just mask the bits and see if anything goes through. If it does,
 * there is a compatibility between the two */
static inline bool is_compatible(struct rt_scheduler_static *rts,
                                 struct thread *t) {
    return rts->capabilities & t->accepted_rt_caps;
}

static inline bool needs_migrate(struct rt_scheduler_static *rts,
                                 struct thread *t) {
    return !is_compatible(rts, t);
}

static size_t find_migration_target(struct thread *t) {
    size_t iter;
    kassert(cpu_mask_popcount(&t->allowed_cpus));
    cpu_mask_for_each(iter, t->allowed_cpus) {
        struct rt_scheduler_percpu *pcpu = global.schedulers[iter]->rt;
        struct rt_scheduler_static *rts = pcpu->active_mapping->static_bptr;
        if (is_compatible(rts, t))
            return iter;
    }

    return SIZE_MAX;
}

static inline bool has_migration_target(struct thread *t) {
    return find_migration_target(t) != SIZE_MAX;
}

static void send_to_compatible_cpu(struct thread *t) {
    size_t go_to = find_migration_target(t);
    struct rt_scheduler *next =
        global.schedulers[go_to]->rt->active_mapping->rts;
    enum irql irql = spin_lock_irq_disable(&next->lock);

    /* TODO: Wrap around thread add/remove to better work with counters and
     * stuff.
     *
     * THIS IS TEMPORARY */
    kassert(is_compatible(next->mapping_source->static_bptr, t));
    next->mapping_source->static_bptr->ops.add_thread(next, t);

    spin_unlock(&next->lock, irql);
}

static bool try_migrate_all_before_switch(struct rt_scheduler *rts,
                                          struct rt_scheduler_static *st,
                                          struct list_head *thread_list) {
    /* All switch events are protected under our beloved rt_global.switch_lock.
     *
     * This means that we are free to read the rt_schedulers of other CPUs
     * without a fear that the rug will be pulled from under us, and this
     * allows us to take our own rt_scheduler lock, and perform proper
     * lock ordering after we move all the threads off of its runqueues.
     */
    enum irql irql = spin_lock_irq_disable(&rts->lock);

    /* Tell the `st` to give us all of its threads so we can take a gander */
    /* TODO: Write a wrapper macro around any operation call */
    st->ops.return_all_threads(rts, thread_list);
    size_t old = rts->thread_count;
    rts->thread_count = 0; /* So as to not confuse the rt_scheduler */

    /* Drop the lock now, we have ownership of all threads (hopefully!!!) */
    spin_unlock(&rts->lock, irql);

    if (list_empty(thread_list)) {
        /* TODO: Find a better way to wrap around assertions made
         * after a RT scheduler op so we don't crash the kernel */
        kassert(old == 0);
        return true; /* No threads, no one can be unhoused! */
    }

    /* Our strategy for this works as follows:
     *
     * First, we do one pass to check and see if all threads have a home
     * after the migration to the new scheduler_static. If some threads
     * do NOT have a home, then we abort the switch, and bail.
     *
     * Otherwise, we will keep the threads that do not need to be
     * migrated on the list, and for every thread that needs to be
     * migrated, we will take it off and away onto another CPU.
     *
     */

    struct thread *iter, *tmp;
    list_for_each_entry_safe(iter, tmp, thread_list, rt_list_node) {
        /* Yikes, it needs to be migrated off and can go nowhere */
        if (needs_migrate(st, iter) && !has_migration_target(iter))
            return false;
    }

    /* OK - now we have verified that everyone has somewhere to go */
    list_for_each_entry_safe(iter, tmp, thread_list, rt_list_node) {
        /* Keep it */
        if (!needs_migrate(st, iter))
            continue;

        /* Bye bye */
        list_del_init(&iter->rt_list_node);
        send_to_compatible_cpu(iter);
    }

    return true;
}

static void re_enqueue_threads(struct rt_scheduler *rts,
                               struct list_head *threads) {
    struct thread *iter, *tmp;

    list_for_each_entry_safe(iter, tmp, threads, rt_list_node) {
        list_del_init(&iter->rt_list_node);
        enum irql irql = spin_lock_irq_disable(&rts->lock);

        /* TODO: remember to wrap these! */
        rts->mapping_source->static_bptr->ops.add_thread(rts, iter);

        spin_unlock(&rts->lock, irql);
    }
}

static void setup_new_rt_scheduler(struct rt_scheduler *rts,
                                   struct rt_scheduler_mapping *rtm) {
    kassert(rts->thread_count == 0);
    kassert(!rts->failed_internal);
    rts->mapping_source = rtm;
    log_trace(rts->log_site, &rts->log_handle, "rts %p setting up by %zu", rts,
              smp_core_id());
}

/* On NUMA systems, we'll iterate to the next closest node
 * if we can't find a struct rt_scheduler for our system.
 *
 * Otherwise, we just scan from 0 to max
 *
 * If we fail to find a struct rt_scheduler, something
 * has gone very very very wrong... */
static struct rt_scheduler *get_new_rt_scheduler(size_t domain) {
    struct list_head *got = NULL;
    if ((got = locked_list_pop_front(&rt_global.sch_pool[domain])))
        goto out;

    if (global.numa_node_count > 1) {
        struct numa_node *node = &global.numa_nodes[domain];
        for (size_t i = 0; i < global.numa_node_count; i++) {
            uint8_t next = node->nodes_by_distance[i];
            if ((got = locked_list_pop_front(&rt_global.sch_pool[next])))
                goto out;
        }
    } else {
        for (size_t i = 0; i < global.domain_count; i++) {
            if ((got = locked_list_pop_front(&rt_global.sch_pool[i])))
                goto out;
        }
    }

out:
    kassert(got);
    return container_of(got, struct rt_scheduler, list);
}

static inline bool check_active(struct rt_scheduler_mapping *rtm) {
    return cpu_mask_test(&rtm->active, smp_core_id());
}

static inline void mark_active(struct rt_scheduler_mapping *rtm) {
    cpu_mask_set(&rtm->active, smp_core_id());
}

static inline void unmark_active(struct rt_scheduler_mapping *rtm) {
    cpu_mask_clear(&rtm->active, smp_core_id());
}

static inline void clear_switch_and_post(struct rt_scheduler_percpu *rts,
                                         enum rt_scheduler_error err) {
    atomic_store_explicit(&rts->switch_code, err, memory_order_release);
    atomic_store_explicit(&rts->switch_into, NULL, memory_order_release);
    semaphore_post(&rts->switch_semaphore);
}

void rt_scheduler_switch() {
    struct rt_scheduler_percpu *pcpu = smp_core_scheduler()->rt;
    struct rt_scheduler_static *into =
        atomic_load_explicit(&pcpu->switch_into, memory_order_acquire);

    /* Nothing to do */
    if (!into)
        return;

    if (!rt_scheduler_static_get(into))
        return clear_switch_and_post(pcpu, RT_SCHEDULER_ERR_NOT_FOUND);

    struct rt_scheduler_static *from = pcpu->active_mapping->static_bptr;
    enum irql girql = spin_lock_irq_disable(&rt_global.switch_lock);
    enum rt_scheduler_error err = RT_SCHEDULER_ERR_OK;
    bool put_into = false;

    struct rt_scheduler_mapping *curr = pcpu->active_mapping;
    struct rt_scheduler_mapping *next = rt_lookup_mapping(into, smp_core());
    bool next_exists = next->rts;
    rt_sched_trace(
        "CPU %zu wants to switch from mapping %zu to mapping %zu (exists: %d)",
        smp_core_id(), curr->id, next->id, next_exists);

    if (curr == next) {
        /* No switch needed, just signal the waiting thread and return */
        rt_sched_trace(
            "No switch needed for CPU %zu, already on the right mapping",
            smp_core_id());

        clear_switch_and_post(pcpu, err);
        spin_unlock(&rt_global.switch_lock, girql);
        rt_scheduler_static_put(into);
        return;
    }

    enum irql irql_curr, irql_next;
    rt_scheduler_acquire_two_mappings(curr, next, &irql_curr, &irql_next);

    /* First we check if we even *can* switch out. If we can't
     * then we leave and return IMPOSSIBLE
     *
     * To do this, we use the following strategy:
     *
     * Check if we are the ONLY CPU for our mapping. If we
     * are NOT, we **always can** switch out.
     *
     * If we ARE the only CPU for our mapping, then we iterate
     * through all the threads, and see if they will all have
     * a safe place to get migrated to. If they DO NOT, then
     * we FAIL the migration with IMPOSSIBLE
     */
    kassert(cpu_mask_test(&curr->active, smp_core_id()));
    bool only_cpu = cpu_mask_popcount(&curr->active) == 1;

    bool can_switch = true;

    struct list_head thread_list;
    INIT_LIST_HEAD(&thread_list);
    if (only_cpu)
        can_switch =
            try_migrate_all_before_switch(curr->rts, into, &thread_list);

    if (!can_switch) {
        put_into = true;
        err = RT_SCHEDULER_ERR_SWITCH_IMPOSSIBLE;
        goto out;
    }

    /* We know we can switch now. All the threads are on our current
     * thread_list.
     *
     * Our approach becomes the following:
     *
     * If we are not the only CPU left, we do not need to reset the scheduler,
     * because someone else is using it. In this case, we just remove ourselves
     * from the active mask of the scheduler. Otherwise, we need to go
     * through the whole process of resetting, etc. etc.
     *
     */

    kassert(check_active(curr));
    unmark_active(curr);
    if (only_cpu)
        reset_scheduler(curr->rts);

    /* The logic for deciding whether or not we need a new rt_scheduler is...
     *
     * If nothing exists for the next mapping (we need to provide an
     * rt_scheduler), AND we are NOT the only CPU for the current
     * mapping, then we will need to get a scheduler from the pool
     */

    /* We are the only CPU and the next mapping has an owner already */
    bool donate = only_cpu && next_exists;
    bool need_new = !only_cpu && !next_exists;
    struct rt_scheduler *next_rts = NULL;

    if (donate)
        locked_list_add(&rt_global.sch_pool[domain_local_id()],
                        &curr->rts->list);

    /* Switch out is completed */
    if (need_new) {
        /* Get a new one, nothing exists for this mapping */
        next_rts = get_new_rt_scheduler(domain_local_id());
    } else {
        next_rts = next->rts;
    }

    /* We are now using the new rt_scheduler */
    pcpu->active_mapping = next;

    if (need_new) {
        /* This list should not have anything because we should
         * not have taken anything from our rt_scheduler. need_new
         * requires us to NOT be the only CPU, so we do not drain
         * the runqueue of our mapping here */
        kassert(list_empty(&thread_list));
        setup_new_rt_scheduler(next_rts, next);
    } else {
        re_enqueue_threads(next_rts, &thread_list);
    }

    /* Drop the old ref, only reachable via success path */
    rt_scheduler_static_put(from);
out:
    /* Post to the thread that sent us the DPC */
    clear_switch_and_post(pcpu, err);
    rt_scheduler_release_two_mappings(curr, next, irql_curr, irql_next);
    spin_unlock(&rt_global.switch_lock, girql);
    if (put_into)
        rt_scheduler_static_put(into);
}

enum rt_scheduler_error
rt_scheduler_switch_cpu(size_t cpu, struct rt_scheduler_static *into) {
    struct rt_scheduler_percpu *pcpu = global.schedulers[cpu]->rt;
    semaphore_wait(&pcpu->switch_semaphore);

    kassert(!atomic_exchange(&pcpu->switch_into, into));
    scheduler_force_resched(global.schedulers[cpu]);

    /* It will signal us now */
    semaphore_wait(&pcpu->switch_semaphore);

    enum rt_scheduler_error ret =
        atomic_load_explicit(&pcpu->switch_code, memory_order_relaxed);

    /* Signal the waiting thread */
    semaphore_post(&pcpu->switch_semaphore);

    return ret;
}
