#include <console/printf.h>
#include <global.h>
#include <log.h>
#include <mem/alloc.h>
#include <mem/numa.h>
#include <sch/sched.h>
#include <smp/domain.h>
#include <stdbool.h>
#include <sync/spinlock.h>

LOG_SITE_DECLARE_DEFAULT(domain);
LOG_HANDLE_DECLARE_DEFAULT(domain);

#define domain_log(lvl, fmt, ...)                                              \
    log(LOG_SITE(domain), LOG_HANDLE(domain), lvl, fmt, ##__VA_ARGS__)

#define domain_err(fmt, ...) domain_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define domain_warn(fmt, ...) domain_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define domain_info(fmt, ...) domain_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define domain_debug(fmt, ...) domain_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define domain_trace(fmt, ...) domain_log(LOG_TRACE, fmt, ##__VA_ARGS__)

static void init_global_domain(uint64_t domain_count) {
    global.domain_count = domain_count;
    global.domains =
        kzalloc(sizeof(struct domain *) * domain_count, ALLOC_PARAMS_DEFAULT);
    if (!global.domains)
        panic("Cannot allocate core domains\n");

    for (size_t i = 0; i < domain_count; i++) {

        /* We align this up to the page so that they can all be
         * migrated later on to pages on each domain... */
        global.domains[i] =
            kzalloc(PAGE_ALIGN_UP(sizeof(struct domain)), ALLOC_PARAMS_DEFAULT);
        if (!global.domains[i])
            panic("Cannot allocate core domain %u\n");

        global.domains[i]->id = i;
    }
}

/* Map 1:1 with NUMA nodes */
static void construct_domains_from_numa_nodes(void) {
    init_global_domain(global.numa_node_count);
    for (size_t i = 0; i < global.numa_node_count; i++) {
        struct numa_node *nn = &global.numa_nodes[i];
        struct domain *cd = global.domains[i];
        struct topology_node *tpn = nn->topo;
        size_t num_cores;
        struct core **arr = topology_get_smts_under_numa(tpn, &num_cores);

        cd->associated_node = nn;
        cd->num_cores = num_cores;
        cd->cores = arr;

        for (size_t i = 0; i < num_cores; i++) {
            arr[i]->domain = cd;
        }
    }
}

static void construct_domains_from_cores(void) {
    size_t n_domains = global.core_count / CORES_PER_DOMAIN;
    size_t remainder = global.core_count % CORES_PER_DOMAIN;

    if (remainder > 0)
        n_domains++; /* one extra for leftover cores */

    init_global_domain(n_domains);

    size_t core_index = 0;
    for (size_t i = 0; i < n_domains; i++) {
        struct domain *cd = global.domains[i];

        cd->associated_node = NULL;

        /* Decide how many cores this domain should get */
        size_t cores_this_domain = CORES_PER_DOMAIN;
        if (i == n_domains - 1 && remainder > 0)
            cores_this_domain = remainder; /* last one gets leftovers */

        cd->num_cores = cores_this_domain;
        cd->cores = kzalloc(sizeof(struct core *) * cores_this_domain,
                            ALLOC_PARAMS_DEFAULT);

        if (!cd->cores)
            panic("Cannot allocate core array for domain %zu\n", i);

        for (size_t j = 0; j < cores_this_domain; j++) {
            global.cores[core_index]->domain = cd;
            cd->cores[j] = global.cores[core_index++];
        }
    }
}

void domain_dump(void) {
    domain_info("Domains (%zu total)", global.domain_count);

    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain *cd = global.domains[i];
        if (cd->associated_node) {
            domain_info(" Domain %zu: Cores = %zu, NUMA node = %zu", i,
                        cd->num_cores, cd->associated_node->topo->id);
        } else {
            domain_info(" Domain %zu: Cores = %zu, NUMA node = <none>", i,
                        cd->num_cores);
        }

        for (size_t j = 0; j < cd->num_cores; j++) {
            if (cd->cores && cd->cores[j]) {
                struct core *c = cd->cores[j];
                domain_info("  Core %zu", c->id);
            } else {
                domain_info("  Core <NULL>");
            }
        }
    }
}

/* If NUMA is present, domains map 1:1 with
 * NUMA nodes. If not, we just group cores into
 * groups of CORES_PER_DOMAIN
 * and construct domains from them. */
void domain_init(void) {
    if (global.numa_node_count > 1) {
        construct_domains_from_numa_nodes();
    } else {
        construct_domains_from_cores();
    }

    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain *domain = global.domains[i];
        for (size_t j = 0; j < domain->num_cores; j++) {
            domain->cores[j]->domain_cpu_id = j;
        }
    }

    domain_dump();
}

void domain_set_cpu_mask(struct cpu_mask *mask, struct domain *domain) {
    for (size_t i = 0; i < domain->num_cores; i++)
        cpu_mask_set(mask, domain->cores[i]->id);
}

/* Create a CPU mask that has all the bits for the domain set */
struct cpu_mask *domain_create_cpu_mask(struct domain *domain) {
    struct cpu_mask *ret = cpu_mask_create();
    if (!ret)
        goto err;

    if (!cpu_mask_init(ret, global.core_count))
        goto err;

    domain_set_cpu_mask(ret, domain);

    return ret;
err:
    if (ret)
        kfree(ret, FREE_PARAMS_DEFAULT);

    return NULL;
}

static void domains_move(void *a, void *b) {
    (void) a, (void) b;
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain *domain = global.domains[i];
        movealloc(domain->id, domain, VMM_FLAG_NONE);
    }
}

bool domain_idle(struct domain *domain) {
    for (size_t i = 0; i < domain->num_cores; i++)
        if (!scheduler_core_idle(domain->cores[i]))
            return false;

    return true;
}

MOVEALLOC_REGISTER_CALL(domain_move, domains_move, /* a = */ NULL,
                        /* b = */ NULL);
