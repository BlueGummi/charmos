#include <asm.h>
#include <console/printf.h>
#include <int/idt.h>
#include <kassert.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/numa.h>
#include <smp/core.h>
#include <smp/smp.h>
#include <smp/topology.h>
#include <stdatomic.h>
#include <sync/spinlock.h>

static struct topology_node *smt_nodes;
static struct topology_node *core_nodes;
static struct topology_node *numa_nodes;
static struct topology_node *package_nodes;
static struct topology_node *llc_nodes;
static struct topology_node machine_node;

#define BOLD_STR(__str) ANSI_BOLD __str ANSI_RESET

static void cpu_mask_print(const struct cpu_mask *m) {
    if (!m->uses_large) {
        k_printf(BOLD_STR("0x%llx"), (uint64_t) m->small);
    } else {
        size_t nwords = (m->nbits + 63) / 64;
        for (size_t i = 0; i < nwords; i++)
            k_printf(BOLD_STR("%016llx"), (uint64_t) m->large[nwords - 1 - i]);
    }
}

#define TOPO_MAKE_STR(__color, __str) (__color __str ANSI_RESET)

static const char *topo_node_str[TOPOLOGY_LEVEL_MAX] = {
    [TOPOLOGY_LEVEL_SMT] = TOPO_MAKE_STR(ANSI_MAGENTA, "SMT"),
    [TOPOLOGY_LEVEL_CORE] = TOPO_MAKE_STR(ANSI_BLUE, "CORE"),
    [TOPOLOGY_LEVEL_LLC] = TOPO_MAKE_STR(ANSI_CYAN, "LLC"),
    [TOPOLOGY_LEVEL_PACKAGE] = TOPO_MAKE_STR(ANSI_GREEN, "PACKAGE"),
    [TOPOLOGY_LEVEL_NUMA] = TOPO_MAKE_STR(ANSI_YELLOW, "NUMA NODE"),
    [TOPOLOGY_LEVEL_MACHINE] = TOPO_MAKE_STR(ANSI_RED, "MACHINE"),
};

const char *topology_level_name(enum topology_level l) {
    return topo_node_str[l];
}

static void print_topology_node(struct topology_node *node, int depth) {
    for (int i = 0; i < depth; i++)
        k_printf("  ");

    const char *level_str = topo_node_str[node->level];

    k_printf("[%s] ID = " ANSI_BOLD "%d" ANSI_RESET ", CPUs = ", level_str,
             node->id);
    cpu_mask_print(&node->cpus);
    k_printf("\n");

    switch (node->level) {
    case TOPOLOGY_LEVEL_MACHINE:
        for (int i = 0; i < global.topology.count[TOPOLOGY_LEVEL_PACKAGE]; i++)
            if (package_nodes[i].parent == node->id)
                print_topology_node(&package_nodes[i], depth + 1);
        break;

    case TOPOLOGY_LEVEL_PACKAGE:
        for (int i = 0; i < global.topology.count[TOPOLOGY_LEVEL_LLC]; i++)
            if (llc_nodes[i].parent == node->id)
                print_topology_node(&llc_nodes[i], depth + 1);
        break;

    case TOPOLOGY_LEVEL_LLC:
        for (int i = 0; i < global.topology.count[TOPOLOGY_LEVEL_NUMA]; i++)
            if (numa_nodes[i].parent == node->id)
                print_topology_node(&numa_nodes[i], depth + 1);
        break;

    case TOPOLOGY_LEVEL_NUMA:
        for (int i = 0; i < global.topology.count[TOPOLOGY_LEVEL_CORE]; i++)
            if (core_nodes[i].parent == node->id)
                print_topology_node(&core_nodes[i], depth + 1);
        break;

    case TOPOLOGY_LEVEL_CORE:
        for (int i = 0; i < global.topology.count[TOPOLOGY_LEVEL_SMT]; i++)
            if (smt_nodes[i].parent == node->id)
                print_topology_node(&smt_nodes[i], depth + 1);
        break;

    default: break;
    }
}

struct cpu_mask *cpu_mask_create(void) {
    return kzalloc(sizeof(struct cpu_mask), ALLOC_PARAMS_DEFAULT);
}

bool cpu_mask_init(struct cpu_mask *m, size_t nbits) {
    m->nbits = nbits;
    if (nbits <= 64) {
        m->uses_large = false;
        return true;
    }

    m->uses_large = true;
    size_t nwords = (nbits + 63) / 64;
    m->large = kzalloc(sizeof(uint64_t) * nwords, ALLOC_PARAMS_DEFAULT);

    if (!m->large)
        return false;

    return true;
}

void cpu_mask_set(struct cpu_mask *m, size_t cpu) {
    if (!m->uses_large) {
        atomic_fetch_or(&m->small, 1ULL << cpu);
    } else {
        atomic_fetch_or(&m->large[cpu / 64], 1ULL << (cpu % 64));
    }
}

void cpu_mask_clear(struct cpu_mask *m, size_t cpu) {
    if (!m->uses_large) {
        atomic_fetch_and(&m->small, ~(1ULL << cpu));
    } else {
        atomic_fetch_and(&m->large[cpu / 64], ~(1ULL << (cpu % 64)));
    }
}

bool cpu_mask_test(const struct cpu_mask *m, size_t cpu) {
    if (!m->uses_large) {
        return (atomic_load(&m->small) >> cpu) & 1ULL;
    } else {
        return (atomic_load(&m->large[cpu / 64]) >> (cpu % 64)) & 1ULL;
    }
}

void cpu_mask_or(struct cpu_mask *dst, const struct cpu_mask *b) {
    if (!dst->uses_large) {
        atomic_fetch_or(&dst->small, atomic_load(&b->small));
    } else {
        size_t nwords = (dst->nbits + 63) / 64;
        for (size_t i = 0; i < nwords; i++)
            atomic_fetch_or(&dst->large[i], atomic_load(&b->large[i]));
    }
}

void cpu_mask_set_all(struct cpu_mask *m) {
    if (!m->uses_large) {
        atomic_store(&m->small, UINT64_MAX);
    } else {
        size_t nwords = (m->nbits + 63) / 64;
        for (size_t i = 0; i < nwords; i++)
            atomic_store(&m->large[i], UINT64_MAX);
    }
}

void cpu_mask_clear_all(struct cpu_mask *m) {
    if (!m->uses_large) {
        atomic_store(&m->small, 0);
    } else {
        size_t nwords = (m->nbits + 63) / 64;
        for (size_t i = 0; i < nwords; i++)
            atomic_store(&m->large[i], 0);
    }
}

bool cpu_mask_empty(const struct cpu_mask *mask) {
    if (!mask->uses_large)
        return atomic_load(&mask->small) == 0;

    size_t nwords = (mask->nbits + 63) / 64;
    for (size_t i = 0; i < nwords; i++)
        if (atomic_load(&mask->large[i]) != 0)
            return false;

    return true;
}

void topology_dump(void) {
    k_info("TOPOLOGY", K_INFO, "Processor topology:");
    print_topology_node(&machine_node, 0);
}

#define PANIC_IF_CPU_MASK_FAILED(op)                                           \
    do {                                                                       \
        if (unlikely(!op))                                                     \
            k_panic("CPU mask allocation failed!\n");                          \
                                                                               \
    } while (0);

static size_t build_smt_nodes(size_t n_cpus) {
    smt_nodes =
        kzalloc(n_cpus * sizeof(struct topology_node), ALLOC_PARAMS_DEFAULT);

    for (size_t i = 0; i < n_cpus; i++) {
        struct core *c = global.cores[i];

        struct topology_node *node = &smt_nodes[i];

        size_t core_index = 0;
        size_t j;
        for_each_cpu_id(j) {
            if (core_nodes[j].core->core_id == c->core_id &&
                core_nodes[j].core->package_id == c->package_id) {
                core_index = j;
                break;
            }
        }

        node->level = TOPOLOGY_LEVEL_SMT;
        node->id = i;
        node->parent = core_index;
        node->core = c;
        c->topo_node = node;
        node->first_child = -1;

        node->nr_children = 0;

        cpu_mask_init(&node->cpus, n_cpus);
        cpu_mask_set(&node->cpus, i);
        cpu_mask_init(&node->idle, n_cpus);
        cpu_mask_set(&node->idle, i);

        struct topology_node *parent_core = &core_nodes[core_index];

        node->parent_node = parent_core;

        if (parent_core->first_child == -1)
            parent_core->first_child = i;

        parent_core->nr_children++;
    }

    return n_cpus;
}

static size_t build_core_nodes(size_t n_cpus) {
    size_t core_count = 0;
    core_nodes =
        kzalloc(n_cpus * sizeof(struct topology_node), ALLOC_PARAMS_DEFAULT);

    for (size_t i = 0; i < n_cpus; i++) {
        struct core *c = global.cores[i];

        bool exists = false;
        for (size_t j = 0; j < core_count; j++) {
            if (core_nodes[j].core->core_id == c->core_id &&
                core_nodes[j].core->package_id == c->package_id) {
                exists = true;
                break;
            }
        }

        if (exists)
            continue;

        struct topology_node *node = &core_nodes[core_count];

        node->level = TOPOLOGY_LEVEL_CORE;
        node->id = c->core_id;
        node->first_child = -1;
        node->nr_children = 0;
        node->core = c;
        node->parent = -1;
        node->parent_node = NULL;

        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&node->cpus, n_cpus));
        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&node->idle, n_cpus));

        for (size_t j = 0; j < n_cpus; j++) {
            struct core *cj = global.cores[j];
            if (cj->core_id == c->core_id && cj->package_id == c->package_id) {
                cpu_mask_set(&node->cpus, j);
                cpu_mask_set(&node->idle, j);
            }
        }

        core_count++;
    }

    return core_count;
}

static bool cpu_mask_intersects(const struct cpu_mask *a,
                                const struct cpu_mask *b) {
    if (!a->uses_large && !b->uses_large) {
        return (a->small & b->small) != 0;
    }

    size_t nwords = ((a->nbits > b->nbits ? a->nbits : b->nbits) + 63) / 64;
    for (size_t i = 0; i < nwords; i++) {
        uint64_t wa = 0, wb = 0;

        if (a->uses_large) {
            if (i < (a->nbits + 63) / 64)
                wa = a->large[i];
        } else if (i == 0) {
            wa = a->small;
        }
        if (b->uses_large) {
            if (i < (b->nbits + 63) / 64)
                wb = b->large[i];
        } else if (i == 0) {
            wb = b->small;
        }

        if ((wa & wb) != 0)
            return true;
    }
    return false;
}

static size_t build_numa_nodes(size_t n_cores, size_t n_llc) {
    size_t max_numa = 0;
    for (size_t i = 0; i < n_cores; i++)
        if (core_nodes[i].core->numa_node > max_numa)
            max_numa = core_nodes[i].core->numa_node;

    size_t n_numa_nodes = max_numa + 1;
    numa_nodes = kzalloc(n_numa_nodes * sizeof(struct topology_node),
                         ALLOC_PARAMS_DEFAULT);

    for (size_t i = 0; i < n_numa_nodes; i++) {
        struct topology_node *numa = &numa_nodes[i];
        numa->level = TOPOLOGY_LEVEL_NUMA;
        numa->id = i;
        numa->parent = -1;
        numa->first_child = -1;
        numa->nr_children = 0;
        numa->core = NULL;

        /* Initialized if there is actually
         * NUMA present (these nodes are not fake) */
        if (global.numa_node_count > 1) {
            numa->data.numa = &global.numa_nodes[i];
            global.numa_nodes[i].topo = numa;
        }

        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&numa->cpus, global.core_count));
        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&numa->idle, global.core_count));
    }

    for (size_t i = 0; i < n_cores; i++) {
        struct core *c = core_nodes[i].core;
        uint32_t numa_id = c->numa_node;
        struct topology_node *numa = &numa_nodes[numa_id];

        core_nodes[i].parent = numa_id;
        core_nodes[i].parent_node = numa;

        if (numa->first_child == -1)
            numa->first_child = i;

        numa->nr_children++;
        cpu_mask_or(&numa->cpus, &core_nodes[i].cpus);
        cpu_mask_or(&numa->idle, &core_nodes[i].idle);
    }

    for (size_t i = 0; i < n_numa_nodes; i++) {
        struct topology_node *numa = &numa_nodes[i];
        if (numa->first_child == -1)
            continue;

        for (size_t j = 0; j < n_llc; j++) {
            struct topology_node *llc = &llc_nodes[j];

            if (cpu_mask_intersects(&llc->cpus, &numa->cpus)) {
                numa->parent = llc->id;
                numa->parent_node = llc;
                numa->data.cache = llc->data.cache;

                if (llc->first_child == -1)
                    llc->first_child = i;

                llc->nr_children++;
                break;
            }
        }
    }

    return n_numa_nodes;
}

static size_t build_llc_nodes(size_t n_cores) {
    llc_nodes =
        kzalloc(n_cores * sizeof(struct topology_node), ALLOC_PARAMS_DEFAULT);
    size_t llc_count = 0;

    for (size_t i = 0; i < n_cores; i++) {
        struct core *c = core_nodes[i].core;
        uint32_t pkg_id = c->package_id;

        if (c->llc.level == 0 || c->llc.type == 0)
            continue;

        bool exists = false;
        for (size_t j = 0; j < llc_count; j++) {
            struct topology_cache_info *existing = llc_nodes[j].data.cache;
            if (existing->level == c->llc.level &&
                existing->type == c->llc.type &&
                existing->size_kb == c->llc.size_kb &&
                llc_nodes[j].parent == pkg_id) { /* Real */
                exists = true;
                cpu_mask_or(&llc_nodes[j].cpus, &core_nodes[i].cpus);
                cpu_mask_or(&llc_nodes[j].idle, &core_nodes[i].idle);
                break;
            }
        }

        if (exists)
            continue;

        struct topology_node *node = &llc_nodes[llc_count];
        node->level = TOPOLOGY_LEVEL_LLC;
        node->id = llc_count;
        node->parent = pkg_id;
        node->core = NULL;
        node->data.cache = &c->llc;

        node->first_child = -1;
        node->nr_children = 0;

        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&node->cpus, global.core_count));
        cpu_mask_or(&node->cpus, &core_nodes[i].cpus);

        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&node->idle, global.core_count));
        cpu_mask_or(&node->idle, &core_nodes[i].idle);

        llc_count++;
    }

    if (llc_count > 0)
        return llc_count;

    /* no LLC info present, mirror packages */
    uint32_t max_pkg_id = 0;
    for (size_t i = 0; i < n_cores; i++)
        if (core_nodes[i].core->package_id > max_pkg_id)
            max_pkg_id = core_nodes[i].core->package_id;

    size_t n_packages = max_pkg_id + 1;

    for (size_t p = 0; p < n_packages; p++) {
        struct topology_node *node = &llc_nodes[llc_count];
        node->level = TOPOLOGY_LEVEL_LLC;
        node->id = llc_count;
        node->parent = p;
        node->core = NULL;
        node->data.cache = NULL;

        node->first_child = -1;
        node->nr_children = 0;

        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&node->cpus, global.core_count));
        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&node->idle, global.core_count));

        for (size_t i = 0; i < n_cores; i++) {

            if (core_nodes[i].core->package_id == p) {
                cpu_mask_or(&node->cpus, &core_nodes[i].cpus);
                cpu_mask_or(&node->idle, &core_nodes[i].idle);
            }
        }

        llc_count++;
    }

    return llc_count;
}

static size_t build_package_nodes(size_t n_cores, size_t n_llc) {

    uint32_t max_pkg_id = 0;

    for (size_t i = 0; i < n_cores; i++)
        if (core_nodes[i].core->package_id > max_pkg_id)
            max_pkg_id = core_nodes[i].core->package_id;

    size_t n_packages = max_pkg_id + 1;
    package_nodes = kzalloc(n_packages * sizeof(struct topology_node),
                            ALLOC_PARAMS_DEFAULT);

    for (size_t i = 0; i < n_packages; i++) {
        struct topology_node *pkg = &package_nodes[i];
        pkg->level = TOPOLOGY_LEVEL_PACKAGE;
        pkg->id = i;
        pkg->parent = 0;
        pkg->first_child = -1;
        pkg->nr_children = 0;
        pkg->core = NULL;
        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&pkg->cpus, global.core_count));
        PANIC_IF_CPU_MASK_FAILED(cpu_mask_init(&pkg->idle, global.core_count));
    }

    for (size_t j = 0; j < n_llc; j++) {
        struct topology_node *llc = &llc_nodes[j];
        uint32_t pkg_id = llc->parent;
        if (pkg_id >= n_packages)
            continue;

        struct topology_node *pkg = &package_nodes[pkg_id];

        if (pkg->first_child == -1)
            pkg->first_child = j;

        llc->parent_node = pkg;
        pkg->parent_node = &machine_node;
        pkg->nr_children++;
        cpu_mask_or(&pkg->cpus, &llc->cpus);
        cpu_mask_or(&pkg->idle, &llc->idle);
    }

    return n_packages;
}

static void build_machine_node(size_t n_packages) {
    machine_node.level = TOPOLOGY_LEVEL_MACHINE;
    machine_node.id = 0;
    machine_node.parent = -1;
    machine_node.first_child = -1;
    machine_node.nr_children = n_packages;
    machine_node.core = NULL;

    PANIC_IF_CPU_MASK_FAILED(
        cpu_mask_init(&machine_node.cpus, global.core_count));
    PANIC_IF_CPU_MASK_FAILED(
        cpu_mask_init(&machine_node.idle, global.core_count));

    for (size_t i = 0; i < n_packages; i++) {
        struct topology_node *pkg = &package_nodes[i];

        if (machine_node.first_child == -1)
            machine_node.first_child = i;

        cpu_mask_or(&machine_node.cpus, &pkg->cpus);
        cpu_mask_or(&machine_node.idle, &pkg->idle);
    }
}

void topology_init(void) {
    size_t n_cpus = global.core_count; /* Logical processor count */

    size_t n_cores = build_core_nodes(n_cpus);
    size_t n_smt = build_smt_nodes(n_cpus);
    size_t n_llc = build_llc_nodes(n_cores);
    size_t n_numa = build_numa_nodes(n_cores, n_llc);
    size_t n_packages = build_package_nodes(n_cores, n_llc);

    build_machine_node(n_packages);

    global.topology.level[TOPOLOGY_LEVEL_SMT] = smt_nodes;
    global.topology.count[TOPOLOGY_LEVEL_SMT] = n_smt;
    global.topology.level[TOPOLOGY_LEVEL_CORE] = core_nodes;
    global.topology.count[TOPOLOGY_LEVEL_CORE] = n_cores;
    global.topology.level[TOPOLOGY_LEVEL_NUMA] = numa_nodes;
    global.topology.count[TOPOLOGY_LEVEL_NUMA] = n_numa;
    global.topology.level[TOPOLOGY_LEVEL_LLC] = llc_nodes;
    global.topology.count[TOPOLOGY_LEVEL_LLC] = n_llc;
    global.topology.level[TOPOLOGY_LEVEL_PACKAGE] = package_nodes;
    global.topology.count[TOPOLOGY_LEVEL_PACKAGE] = n_packages;
    global.topology.level[TOPOLOGY_LEVEL_MACHINE] = &machine_node;
    global.topology.count[TOPOLOGY_LEVEL_MACHINE] = 1;

    topology_dump();
}

struct core **topology_get_smts_under_numa(struct topology_node *numa,
                                           size_t *count) {
    size_t total = 0;
    struct core **smts = NULL;
    if (!numa || numa->level != TOPOLOGY_LEVEL_NUMA)
        goto out;

    for (int32_t i = 0; i < numa->nr_children; i++) {
        struct topology_node *core_node = &core_nodes[numa->first_child + i];
        total += core_node->nr_children;
    }

    if (total == 0)
        goto out;

    smts = kmalloc(sizeof(struct core *) * total, ALLOC_PARAMS_DEFAULT);
    if (!smts)
        k_panic("Could not allocate array for NUMA SMTs\n");

    size_t idx = 0;
    for (int32_t i = 0; i < numa->nr_children; i++) {
        struct topology_node *core_node = &core_nodes[numa->first_child + i];
        for (int32_t j = 0; j < core_node->nr_children; j++) {
            struct topology_node *smt_node =
                &smt_nodes[core_node->first_child + j];
            smts[idx++] = smt_node->core;
        }
    }

out:
    *count = total;
    return smts;
}

void topology_mark_core_idle(size_t cpu_id, bool idle) {
    if (!global.topology.level[TOPOLOGY_LEVEL_MACHINE])
        return;

    struct topology_node *smt = &smt_nodes[cpu_id];

    struct topology_node *node = smt;
    while (node) {
        if (idle)
            cpu_mask_set(&node->idle, cpu_id);
        else
            cpu_mask_clear(&node->idle, cpu_id);

        node = node->parent_node;
    }
}

struct core *topology_find_idle_core(struct core *local_core,
                                     enum topology_level max_search) {
    kassert(max_search >
            TOPOLOGY_LEVEL_SMT); /* 'SMT' will be the core itself.
                                  * It has no neighbors, and thus
                                  * cannot be searched through (one core) */

    struct topology_node *smt_node = local_core->topo_node; /* Direct node  */
    struct topology_node *core_node = smt_node->parent_node;
    struct topology_node *numa_node = core_node->parent_node;
    struct topology_node *llc_node = numa_node->parent_node;
    struct topology_node *pkg_node = llc_node->parent_node;

    /* First try SMT siblings */
    for (int32_t i = 0; i < core_node->nr_children; i++) {
        struct topology_node *sibling = &smt_nodes[core_node->first_child + i];
        if (!cpu_mask_empty(&sibling->idle))
            return sibling->core;
    }

    /* Not allowed to search to NUMA */
    if (max_search < TOPOLOGY_LEVEL_NUMA)
        return NULL;

    /* Next try NUMA siblings */
    for (int32_t i = 0; i < numa_node->nr_children; i++) {
        struct topology_node *core = &core_nodes[numa_node->first_child + i];
        if (!cpu_mask_empty(&core->idle))
            return core->core;
    }

    if (max_search < TOPOLOGY_LEVEL_LLC)
        return NULL;

    for (int32_t i = 0; i < llc_node->nr_children; i++) {
        struct topology_node *core = &core_nodes[llc_node->first_child + i];
        if (!cpu_mask_empty(&core->idle))
            return core->core;
    }

    /* Finally do a full CPU scan */
    for (int32_t i = 0; i < pkg_node->nr_children; i++) {
        struct topology_node *llc = &llc_nodes[pkg_node->first_child + i];
        if (cpu_mask_empty(&llc->idle))
            continue;

        for (int32_t j = 0; j < smt_nodes->nr_children; j++) {
            struct topology_node *smt = &smt_nodes[llc->first_child + j];
            if (!cpu_mask_empty(&smt->idle))
                return smt->core;
        }
    }

    size_t i;
    for_each_cpu_id(i) {
        struct topology_node *smt = &smt_nodes[i];
        if (!cpu_mask_empty(&smt->idle))
            return smt->core;
    }

    return NULL;
}
