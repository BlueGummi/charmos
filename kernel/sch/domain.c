#include <console/panic.h>
#include <global.h>
#include <kassert.h>
#include <log.h>
#include <mem/alloc.h>
#include <sch/domain.h>
#include <string.h>

static void sched_mask_clone(struct cpu_mask *dst, const struct cpu_mask *src) {
    cpu_mask_init(dst, src->nbits);

    if (src->uses_large) {
        size_t words = CPU_MASK_WORDS(src->nbits);
        memcpy(dst->large, src->large, words * sizeof(uint64_t));
    } else {
        dst->small = src->small;
    }
}

static bool cpu_mask_intersects(const struct cpu_mask *a,
                                const struct cpu_mask *b) {
    size_t nbits = a->nbits;
    for (size_t i = 0; i < nbits; ++i)
        if (cpu_mask_test(a, i) && cpu_mask_test(b, i))
            return true;

    return false;
}

static struct scheduler_domain *
build_domain_for_level(enum topology_level lvl) {
    struct topology *t = &global.topology;

    size_t n = t->count[lvl];
    struct topology_node *nodes = t->level[lvl];

    struct scheduler_domain *d = kzalloc(sizeof(*d), ALLOC_PARAMS_DEFAULT);
    d->level = lvl;
    d->ngroups = n;
    d->groups =
        kzalloc(sizeof(struct scheduler_group) * n, ALLOC_PARAMS_DEFAULT);

    for (size_t i = 0; i < n; i++) {
        struct topology_node *node = &nodes[i];

        /* clone cpu masks into group */
        sched_mask_clone(&d->groups[i].cpus, &node->cpus);
        sched_mask_clone(&d->groups[i].idle, &node->idle);

        d->groups[i].topo_index = i;

        d->groups[i].parent_index = -1;

        /* dummy capacity for now */

        d->groups[i].capacity = 1;
    }

    return d;
}

static void link_parent_groups(struct scheduler_domain *child,
                               struct scheduler_domain *parent) {
    child->parent = parent;

    for (size_t g = 0; g < child->ngroups; g++) {

        struct scheduler_group *cg = &child->groups[g];

        /* find parent group whose cpus intersect */
        for (size_t pg = 0; pg < parent->ngroups; pg++) {
            struct scheduler_group *pgp = &parent->groups[pg];
            if (cpu_mask_intersects(&cg->cpus, &pgp->cpus)) {
                cg->parent_index = pg;
                break;
            }
        }
    }
}

static void map_cpus_to_groups(void) {
    struct core *c;
    for_each_cpu_struct(c) {
        struct core *c = global.cores[__id];

        for (size_t i = 0; i < TOPOLOGY_LEVEL_MAX; i++) {
            struct scheduler_domain *d = global.scheduler_domains[i];
            c->domains[i] = d;

            /* find group for this CPU */
            int found = -1;
            for (size_t g = 0; g < d->ngroups; g++) {
                if (cpu_mask_test(&d->groups[g].cpus, __id)) {
                    found = g;
                    break;
                }
            }
            kassert(found != -1);
            c->group_index[i] = found;
        }
    }
}

void cpu_mask_print(const struct cpu_mask *m, char *buf, size_t buflen) {
    size_t pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "{");

    size_t last = 0;
    for (size_t cpu = 0; cpu < m->nbits; cpu++)
        if (cpu_mask_test(m, cpu))
            if (last < cpu)
                last = cpu;

    for (size_t cpu = 0; cpu < m->nbits; cpu++) {
        if (cpu_mask_test(m, cpu)) {
            if (cpu == last)
                pos += snprintf(buf + pos, buflen - pos, "%zu", cpu);
            else
                pos += snprintf(buf + pos, buflen - pos, "%zu,", cpu);
        }
    }
    snprintf(buf + pos, buflen - pos, "}");
}

void scheduler_domains_dump(void) {
    for (size_t lvl = 0; lvl < TOPOLOGY_LEVEL_MAX; lvl++) {
        struct scheduler_domain *d = global.scheduler_domains[lvl];

        log_msg(LOG_INFO, "SCHEDULER DOMAIN %zu (%s), %zu groups", lvl,
                topology_level_name(lvl), d->ngroups);

        for (size_t g = 0; g < d->ngroups; g++) {
            struct scheduler_group *grp = &d->groups[g];

            char buf1[256], buf2[256];
            cpu_mask_print(&grp->cpus, buf1, sizeof(buf1));

            cpu_mask_print(&grp->idle, buf2, sizeof(buf2));

            printf(
                "  Group %zu: CPUs = %s Idle = %s Parent = %d Capacity = %d\n",
                g, buf1, buf2, grp->parent_index, grp->capacity);
        }

        printf("\n");
    }
}

void scheduler_domains_init(void) {
    for (size_t i = 0; i < TOPOLOGY_LEVEL_MAX; i++) {
        global.scheduler_domains[i] = build_domain_for_level(i);
    }

    for (size_t i = 0; i + 1 < TOPOLOGY_LEVEL_MAX; i++) {
        link_parent_groups(global.scheduler_domains[i],
                           global.scheduler_domains[i + 1]);
    }

    map_cpus_to_groups();

    scheduler_domains_dump();

    global.scheduler_domains_ready = true;
}

struct scheduler_group *scheduler_domain_find_sibling_group(struct core *c,
                                                            size_t domain_idx) {
    struct scheduler_domain *d = c->domains[domain_idx];
    size_t my_g = c->group_index[domain_idx];

    for (size_t g = 0; g < d->ngroups; g++)
        if (g != my_g)
            return &d->groups[g];

    return NULL;
}

int32_t scheduler_group_find_idle_cpu(struct scheduler_group *g) {
    for (size_t cpu = 0; cpu < g->cpus.nbits; cpu++)
        if (cpu_mask_test(&g->cpus, cpu) && cpu_mask_test(&g->idle, cpu))
            return cpu;

    return -1;
}

void scheduler_domain_mark_self_idle(bool idle) {
    if (!global.scheduler_domains_ready)
        return;

    struct core *c = smp_core();
    size_t cpu = smp_core_id();

    for (size_t lvl = 0; lvl < TOPOLOGY_LEVEL_MAX; lvl++) {
        struct scheduler_domain *d = c->domains[lvl];
        size_t g = c->group_index[lvl];
        kassert(c->group_index[lvl] >= 0);
        kassert((size_t) c->group_index[lvl] < d->ngroups);

        if (idle)
            cpu_mask_set(&d->groups[g].idle, cpu);
        else
            cpu_mask_clear(&d->groups[g].idle, cpu);
    }
}

int32_t scheduler_find_idle_cpu_near(struct core *from) {
    if (!global.scheduler_domains_ready)
        return -1;

    for (size_t lvl = TOPOLOGY_LEVEL_SMT; lvl < TOPOLOGY_LEVEL_MAX; lvl++) {
        struct scheduler_domain *d = from->domains[lvl];
        size_t g = from->group_index[lvl];

        struct scheduler_group *grp = &d->groups[g];
        int cpu = scheduler_group_find_idle_cpu(grp);
        if (cpu >= 0)
            return cpu;
    }
    return -1;
}

int32_t scheduler_push_target(struct core *from) {
    if (!global.scheduler_domains_ready)
        return -1;

    for (int32_t lvl = TOPOLOGY_LEVEL_MAX - 1; lvl >= 0; lvl--) {
        struct scheduler_domain *d = from->domains[lvl];

        for (size_t g = 0; g < d->ngroups; g++) {
            if ((int32_t) g == from->group_index[lvl])
                continue;

            int32_t cpu = scheduler_group_find_idle_cpu(&d->groups[g]);
            if (cpu >= 0)
                return cpu;
        }
    }
    return -1;
}
