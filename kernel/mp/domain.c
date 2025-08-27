#include <charmos.h>
#include <console/panic.h>
#include <mem/alloc.h>
#include <mp/domain.h>
#include <stdbool.h>
#include <sync/spin_lock.h>

static void init_global_domain(uint64_t domain_count) {
    global.domain_count = domain_count;
    global.core_domains = kzalloc(sizeof(struct core_domain *) * domain_count);
    if (!global.core_domains)
        k_panic("Cannot allocate core domains\n");

    for (size_t i = 0; i < domain_count; i++) {
        global.core_domains[i] = kzalloc(sizeof(struct core_domain));
        if (!global.core_domains[i])
            k_panic("Cannot allocate core domain %u\n");
    }
}

/* Map 1:1 with NUMA nodes */
static void construct_domains_from_numa_nodes(void) {
    init_global_domain(global.numa_node_count);
    for (size_t i = 0; i < global.numa_node_count; i++) {
        struct numa_node *nn = &global.numa_nodes[i];
        struct core_domain *cd = global.core_domains[i];
        struct topology_node *tpn = nn->topo;
        size_t num_cores;
        struct core **arr = topo_get_smts_under_numa(tpn, &num_cores);

        cd->associated_node = nn;
        cd->num_cores = num_cores;
        cd->cores = arr;
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
        struct core_domain *cd = global.core_domains[i];

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
            cd->cores[j] = global.cores[core_index++];
        }
    }
}

void core_domain_dump(void) {
    k_printf("=== Core Domains (%zu total) ===\n", global.domain_count);

    for (size_t i = 0; i < global.domain_count; i++) {
        struct core_domain *cd = global.core_domains[i];
        if (!cd) {
            k_printf(" [Domain %zu] <NULL>\n", i);
            continue;
        }

        k_printf(" [Domain %zu]\n", i);

        if (cd->associated_node) {
            k_printf("   NUMA node: %zu\n", cd->associated_node->topo->id);
        } else {
            k_printf("   NUMA node: <none>\n");
        }

        k_printf("   cores: %zu\n", cd->num_cores);
        for (size_t j = 0; j < cd->num_cores; j++) {
            if (cd->cores && cd->cores[j]) {
                struct core *c = cd->cores[j];
                k_printf("     core[%zu] = id %zu\n", j, c->id);
            } else {
                k_printf("     core[%zu] = <NULL>\n", j);
            }
        }
    }
}

/* If NUMA is present, domains map 1:1 with
 * NUMA nodes. If not, we just group cores into
 * groups of 4 and construct domains from them. */
void core_domain_init(void) {
    if (global.numa_node_count > 1) {
        construct_domains_from_numa_nodes();
    } else {
        construct_domains_from_cores();
    }

    core_domain_dump();
}
