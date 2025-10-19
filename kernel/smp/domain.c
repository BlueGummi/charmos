#include <charmos.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <smp/domain.h>
#include <stdbool.h>
#include <sync/spinlock.h>

static void init_global_domain(uint64_t domain_count) {
    global.domain_count = domain_count;
    global.domains = kzalloc(sizeof(struct domain *) * domain_count);
    if (!global.domains)
        k_panic("Cannot allocate core domains\n");

    for (size_t i = 0; i < domain_count; i++) {
        global.domains[i] = kzalloc(sizeof(struct domain));
        if (!global.domains[i])
            k_panic("Cannot allocate core domain %u\n");

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
        cd->cores = kzalloc(sizeof(struct core *) * cores_this_domain);

        if (!cd->cores)
            k_panic("Cannot allocate core array for domain %zu\n", i);

        for (size_t j = 0; j < cores_this_domain; j++) {
            global.cores[core_index]->domain = cd;
            cd->cores[j] = global.cores[core_index++];
        }
    }
}

#define domain_info(fmt, ...) k_info("DOMAIN", K_INFO, fmt, ##__VA_ARGS__)

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
 * groups of 4 and construct domains from them. */
void domain_init(void) {
    if (global.numa_node_count > 1) {
        construct_domains_from_numa_nodes();
    } else {
        construct_domains_from_cores();
    }

    domain_dump();
}
