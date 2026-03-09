#include <mem/alloc.h>
#include <sch/rt_sched.h>
#include <smp/topology.h>
#include <sync/spinlock.h>

#include "internal.h"

struct rt_scheduler_global rt_scheduler_global = {0};

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

    ret->id = id;
    ret->static_backpointer = rts;
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
    /* Called if the mapping allocations fail at init */
    struct rbt_node *iter, *tmp;
    rbt_for_each_safe(iter, tmp, &rts->mappings_internal) {
        rbt_delete(&rts->mappings_internal, iter);
        struct rt_scheduler_mapping *m =
            container_of(iter, struct rt_scheduler_mapping, tree_node);
        kfree(m, FREE_PARAMS_DEFAULT);
    }
}

struct rt_scheduler_mapping *rt_lookup_mapping(struct rt_scheduler_static *rts,
                                               struct core *c) {
    rt_domain_id_t id = rts->ops.domain_id_for_cpu(c);
    struct rbt_node *found = rbt_search(&rts->mappings_internal, id);
    if (!found)
        return NULL;

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

enum rt_scheduler_error
rt_load_scheduler_static(struct rt_scheduler_static *rts) {
    enum rt_scheduler_error err;
    enum irql irql = spin_lock(&rts->lock_internal);


    INIT_LIST_HEAD(&rts->list_internal);
    locked_list_add(&rt_scheduler_global.list, &rts->list_internal);

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

    err = rts->ops.on_load(rts);

done:
    if (err == RT_SCHEDULER_ERR_OK)
        rts->state = RT_SCHEDULER_STATIC_LOADED;

    spin_unlock(&rts->lock_internal, irql);
    return err;
}

enum rt_scheduler_error
rt_unload_scheduler_static(struct rt_scheduler_static *rts) {


}
