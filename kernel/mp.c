#include <acpi/lapic.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <compiler.h>
#include <console/printf.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spin_lock.h>

static uint64_t cr3 = 0;
static atomic_char cr3_ready = 0;
static struct spinlock wakeup_lock = SPINLOCK_INIT;
static atomic_uint cores_awake = 0;

static void detect_llc(struct topo_cache_info *llc) {
    uint32_t eax, ebx, ecx, edx;
    for (uint32_t idx = 0;; idx++) {

        cpuid_count(4, idx, &eax, &ebx, &ecx, &edx);

        uint32_t cache_type = eax & 0x1F;
        if (cache_type == 0)
            break;

        uint32_t cache_level = (eax >> 5) & 0x7;
        if (cache_level != 3)
            continue;

        llc->level = cache_level;
        llc->type = cache_type;
        llc->line_size = (ebx & 0xFFF) + 1;
        llc->cores_sharing = ((eax >> 14) & 0xFFF) + 1;

        uint32_t sets = ecx + 1;
        uint32_t ways = ((ebx >> 22) & 0x3FF) + 1;
        llc->size_kb = (ways * sets * llc->line_size) / 1024;

        break;
    }
}

static void init_smt_info(struct core *c) {
    uint32_t eax, ebx, ecx, edx;

    uint32_t smt_width = 0;
    uint32_t core_width = 0;
    uint32_t apic_id;

    for (uint32_t level = 0;; level++) {
        cpuid_count(0xB, level, &eax, &ebx, &ecx, &edx);
        uint32_t level_type = (ecx >> 8) & 0xFF;
        if (level_type == 0)
            break;

        if (level_type == 1) {
            smt_width = eax & 0x1F;
        } else if (level_type == 2) {
            core_width = eax & 0x1F;
        }
    }

    apic_id = c->id;
    c->package_id = c->id >> core_width;
    c->smt_mask = (1 << smt_width) - 1;
    c->smt_id = apic_id & c->smt_mask;
    c->core_id = (apic_id >> smt_width) & ((1 << (core_width - smt_width)) - 1);
}

static struct core *setup_cpu(uint64_t cpu) {
    struct core *c = kzalloc(sizeof(struct core));
    if (!c)
        k_panic("Core %d could not allocate space for struct\n", cpu);
    c->id = cpu;
    init_smt_info(c);
    detect_llc(&c->llc);

    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores[cpu] = c;
    return c;
}

static inline void set_core_awake(void) {
    atomic_fetch_add_explicit(&cores_awake, 1, memory_order_release);
    if (atomic_load_explicit(&cores_awake, memory_order_acquire) ==
        (global.core_count - 1)) {
        global.current_bootstage = BOOTSTAGE_MID_MP;
    }
}

void wakeup() {
    bool ints = spin_lock(&wakeup_lock);
    smap_init();
    serial_init();

    while (!cr3_ready)
        cpu_relax();

    asm volatile("mov %0, %%cr3" ::"r"(cr3));

    uint64_t cpu = lapic_get_id();

    gdt_install();
    idt_load();

    setup_cpu(cpu);
    lapic_timer_init(cpu);
    set_core_awake();
    spin_unlock(&wakeup_lock, ints);

    enable_interrupts();
    scheduler_yield();
    while (1)
        wait_for_interrupt();
}

void mp_wakeup_processors(struct limine_mp_response *mpr) {
    for (uint64_t i = 0; i < mpr->cpu_count; i++)
        mpr->cpus[i]->goto_address = wakeup;
}

void mp_complete_init() {
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    atomic_store(&cr3_ready, 1);
    if (global.core_count == 1)
        return;

    /* I know, I know, mmio is used here to force the read */
    while (mmio_read_8((uint8_t *) &global.current_bootstage) !=
           BOOTSTAGE_MID_MP)
        ;
}

void mp_setup_bsp() {
    struct core *c = kmalloc(sizeof(struct core));
    if (!c)
        k_panic("Could not allocate space for core structure on BSP");

    c->current_thread = kzalloc(sizeof(struct thread));
    if (unlikely(!c->current_thread))
        k_panic("Could not allocate space for BSP's current thread");

    c->id = 0;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores = kmalloc(sizeof(struct core *) * global.core_count);

    if (unlikely(!global.cores))
        k_panic("Could not allocate space for global core structures");

    global.cores[0] = c;
    init_smt_info(c);
    detect_llc(&c->llc);
}

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

static const char *topo_node_str[TL_MAX] = {
    [TL_SMT] = TOPO_MAKE_STR(ANSI_MAGENTA, "SMT"),
    [TL_CORE] = TOPO_MAKE_STR(ANSI_BLUE, "CORE"),
    [TL_LLC] = TOPO_MAKE_STR(ANSI_CYAN, "LLC"),
    [TL_PACKAGE] = TOPO_MAKE_STR(ANSI_GREEN, "PACKAGE"),
    [TL_NUMA] = TOPO_MAKE_STR(ANSI_YELLOW, "NUMA NODE"),
    [TL_MACHINE] = TOPO_MAKE_STR(ANSI_RED, "MACHINE"),
};

static void print_topology_node(struct topology_node *node, int depth) {
    for (int i = 0; i < depth; i++)
        k_printf("  ");

    const char *level_str = topo_node_str[node->level];

    k_printf("[%s] ID = " ANSI_BOLD "%d" ANSI_RESET ", CPUs = ", level_str,
             node->id);
    cpu_mask_print(&node->cpus);
    k_printf("\n");

    switch (node->level) {
    case TL_MACHINE:
        for (int i = 0; i < global.cpu_topology.count[TL_PACKAGE]; i++)
            if (package_nodes[i].parent == node->id)
                print_topology_node(&package_nodes[i], depth + 1);
        break;

    case TL_PACKAGE:
        for (int i = 0; i < global.cpu_topology.count[TL_LLC]; i++)
            if (llc_nodes[i].parent == node->id)
                print_topology_node(&llc_nodes[i], depth + 1);
        break;

    case TL_LLC:
        for (int i = 0; i < global.cpu_topology.count[TL_NUMA]; i++)
            if (numa_nodes[i].parent == node->id)
                print_topology_node(&numa_nodes[i], depth + 1);
        break;

    case TL_NUMA:
        for (int i = 0; i < global.cpu_topology.count[TL_CORE]; i++)

            if (core_nodes[i].parent == node->id)

                print_topology_node(&core_nodes[i], depth + 1);
        break;

    case TL_CORE:
        for (int i = 0; i < global.cpu_topology.count[TL_SMT]; i++)
            if (smt_nodes[i].parent == node->id)
                print_topology_node(&smt_nodes[i], depth + 1);
        break;

    default: break;
    }
}

static void cpu_mask_init(struct cpu_mask *m, size_t nbits) {
    m->nbits = nbits;
    if (nbits <= 64) {
        m->uses_large = false;
        m->small = 0;
    } else {
        m->uses_large = true;
        size_t nwords = (nbits + 63) / 64;
        m->large = kzalloc(sizeof(uint64_t) * nwords);
    }
}

void cpu_mask_set(struct cpu_mask *m, size_t cpu) {
    if (!m->uses_large) {
        m->small |= (1ULL << cpu);
    } else {
        m->large[cpu / 64] |= (1ULL << (cpu % 64));
    }
}

void cpu_mask_clear(struct cpu_mask *m, size_t cpu) {
    if (!m->uses_large) {
        m->small &= ~(1ULL << cpu);
    } else {
        m->large[cpu / 64] &= ~(1ULL << (cpu % 64));
    }
}

bool cpu_mask_test(const struct cpu_mask *m, size_t cpu) {
    if (!m->uses_large) {
        return (m->small >> cpu) & 1ULL;
    } else {
        return (m->large[cpu / 64] >> (cpu % 64)) & 1ULL;
    }
}

void cpu_mask_or(struct cpu_mask *dst, const struct cpu_mask *a,
                 const struct cpu_mask *b) {
    if (!dst->uses_large) {
        dst->small = a->small | b->small;
    } else {
        size_t nwords = (dst->nbits + 63) / 64;
        for (size_t i = 0; i < nwords; i++)
            dst->large[i] = a->large[i] | b->large[i];
    }
}

void topology_dump(void) {
    k_info("TOPOLOGY", K_INFO, "Processor topology:");
    print_topology_node(&machine_node, 0);
}

static size_t build_smt_nodes(size_t n_cpus) {
    smt_nodes = kzalloc(n_cpus * sizeof(struct topology_node));

    for (size_t i = 0; i < n_cpus; i++) {
        struct core *c = global.cores[i];

        struct topology_node *node = &smt_nodes[i];

        size_t core_index = 0;
        for (size_t j = 0; j < global.core_count; j++) {
            if (core_nodes[j].core->core_id == c->core_id &&
                core_nodes[j].core->package_id == c->package_id) {
                core_index = j;
                break;
            }
        }

        node->level = TL_SMT;
        node->id = i;
        node->parent = core_index;
        node->core = c;
        node->first_child = -1;

        node->nr_children = 0;

        cpu_mask_init(&node->cpus, n_cpus);
        cpu_mask_set(&node->cpus, i);
        cpu_mask_init(&node->idle, n_cpus);
        cpu_mask_set(&node->idle, i);

        struct topology_node *parent_core = &core_nodes[core_index];
        if (parent_core->first_child == -1)
            parent_core->first_child = i;

        parent_core->nr_children++;
    }

    return n_cpus;
}

static size_t build_core_nodes(size_t n_cpus) {
    size_t core_count = 0;
    core_nodes = kzalloc(n_cpus * sizeof(struct topology_node));

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

        node->level = TL_CORE;
        node->id = c->core_id;
        node->first_child = -1;
        node->nr_children = 0;
        node->core = c;

        cpu_mask_init(&node->cpus, n_cpus);
        cpu_mask_init(&node->idle, n_cpus);

        for (size_t j = 0; j < n_cpus; j++) {
            struct core *cj = global.cores[j];
            if (cj->core_id == c->core_id && cj->package_id == c->package_id) {
                cpu_mask_set(&node->cpus, j);
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
    for (size_t i = 0; i < n_cores; i++) {
        if (core_nodes[i].core->numa_node > max_numa)
            max_numa = core_nodes[i].core->numa_node;
    }

    size_t n_numa_nodes = max_numa + 1;
    numa_nodes = kzalloc(n_numa_nodes * sizeof(struct topology_node));

    for (size_t i = 0; i < n_numa_nodes; i++) {
        struct topology_node *numa = &numa_nodes[i];
        numa->level = TL_NUMA;
        numa->id = i;
        numa->parent = -1;
        numa->first_child = -1;
        numa->nr_children = 0;
        numa->core = NULL;
        cpu_mask_init(&numa->cpus, global.core_count);
        cpu_mask_init(&numa->idle, global.core_count);
    }

    for (size_t i = 0; i < n_cores; i++) {
        struct core *c = core_nodes[i].core;
        uint32_t numa_id = c->numa_node;
        struct topology_node *numa = &numa_nodes[numa_id];

        core_nodes[i].parent = numa_id;

        if (numa->first_child == -1)
            numa->first_child = i;

        numa->nr_children++;
        cpu_mask_or(&numa->cpus, &numa->cpus, &core_nodes[i].cpus);
        cpu_mask_or(&numa->idle, &numa->idle, &core_nodes[i].idle);
    }

    for (size_t i = 0; i < n_numa_nodes; i++) {
        struct topology_node *numa = &numa_nodes[i];
        if (numa->first_child == -1)
            continue;

        for (size_t j = 0; j < n_llc; j++) {
            struct topology_node *llc = &llc_nodes[j];

            if (cpu_mask_intersects(&llc->cpus, &numa->cpus)) {
                numa->parent = llc->id;
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
    llc_nodes = kzalloc(n_cores * sizeof(struct topology_node));
    size_t llc_count = 0;

    for (size_t i = 0; i < n_cores; i++) {
        struct core *c = core_nodes[i].core;
        uint32_t pkg_id = c->package_id;

        if (c->llc.level == 0 || c->llc.type == 0)
            continue;

        bool exists = false;
        for (size_t j = 0; j < llc_count; j++) {
            struct topo_cache_info *existing = llc_nodes[j].data.cache;

            if (existing->level == c->llc.level &&
                existing->type == c->llc.type &&
                existing->size_kb == c->llc.size_kb &&
                llc_nodes[j].parent == pkg_id) {

                exists = true;
                cpu_mask_or(&llc_nodes[j].cpus, &llc_nodes[j].cpus,

                            &core_nodes[i].cpus);
                cpu_mask_or(&llc_nodes[j].idle, &llc_nodes[j].idle,
                            &core_nodes[i].idle);
                break;
            }
        }

        if (exists)
            continue;

        struct topology_node *node = &llc_nodes[llc_count];
        node->level = TL_LLC;
        node->id = llc_count;
        node->parent = pkg_id;
        node->core = NULL;
        node->data.cache = &c->llc;

        node->first_child = -1;
        node->nr_children = 0;

        cpu_mask_init(&node->cpus, global.core_count);
        cpu_mask_or(&node->cpus, &node->cpus, &core_nodes[i].cpus);

        cpu_mask_init(&node->idle, global.core_count);
        cpu_mask_or(&node->idle, &node->idle, &core_nodes[i].idle);

        llc_count++;
    }

    /* no LLC info present, mirror packages */
    if (llc_count == 0) {
        uint32_t max_pkg_id = 0;
        for (size_t i = 0; i < n_cores; i++)
            if (core_nodes[i].core->package_id > max_pkg_id)
                max_pkg_id = core_nodes[i].core->package_id;

        size_t n_packages = max_pkg_id + 1;

        for (size_t p = 0; p < n_packages; p++) {
            struct topology_node *node = &llc_nodes[llc_count];
            node->level = TL_LLC;
            node->id = llc_count;
            node->parent = p;
            node->core = NULL;
            node->data.cache = NULL;

            node->first_child = -1;
            node->nr_children = 0;

            cpu_mask_init(&node->cpus, global.core_count);
            cpu_mask_init(&node->idle, global.core_count);

            for (size_t i = 0; i < n_cores; i++) {

                if (core_nodes[i].core->package_id == p) {
                    cpu_mask_or(&node->cpus, &node->cpus, &core_nodes[i].cpus);
                    cpu_mask_or(&node->idle, &node->idle, &core_nodes[i].idle);
                }
            }

            llc_count++;
        }
    }

    return llc_count;
}

static size_t build_package_nodes(size_t n_cores, size_t n_llc) {

    uint32_t max_pkg_id = 0;

    for (size_t i = 0; i < n_cores; i++)
        if (core_nodes[i].core->package_id > max_pkg_id)
            max_pkg_id = core_nodes[i].core->package_id;

    size_t n_packages = max_pkg_id + 1;
    package_nodes = kzalloc(n_packages * sizeof(struct topology_node));

    for (size_t i = 0; i < n_packages; i++) {
        struct topology_node *pkg = &package_nodes[i];
        pkg->level = TL_PACKAGE;
        pkg->id = i;
        pkg->parent = 0;
        pkg->first_child = -1;
        pkg->nr_children = 0;
        pkg->core = NULL;
        cpu_mask_init(&pkg->cpus, global.core_count);
        cpu_mask_init(&pkg->idle, global.core_count);
    }

    for (size_t j = 0; j < n_llc; j++) {
        struct topology_node *llc = &llc_nodes[j];
        uint32_t pkg_id = llc->parent;
        if (pkg_id >= n_packages)
            continue;

        struct topology_node *pkg = &package_nodes[pkg_id];

        if (pkg->first_child == -1)
            pkg->first_child = j;

        pkg->nr_children++;
        cpu_mask_or(&pkg->cpus, &pkg->cpus, &llc->cpus);
        cpu_mask_or(&pkg->idle, &pkg->idle, &llc->idle);
    }

    return n_packages;
}

static void build_machine_node(size_t n_packages) {
    machine_node.level = TL_MACHINE;
    machine_node.id = 0;
    machine_node.parent = -1;
    machine_node.first_child = -1;
    machine_node.nr_children = n_packages;
    machine_node.core = NULL;

    cpu_mask_init(&machine_node.cpus, global.core_count);
    cpu_mask_init(&machine_node.idle, global.core_count);

    for (size_t i = 0; i < n_packages; i++) {
        struct topology_node *pkg = &package_nodes[i];

        if (machine_node.first_child == -1)
            machine_node.first_child = i;

        cpu_mask_or(&machine_node.cpus, &machine_node.cpus, &pkg->cpus);
        cpu_mask_or(&machine_node.idle, &machine_node.idle, &pkg->idle);
    }
}

void topology_init(void) {
    size_t n_cpus = global.core_count;

    size_t n_cores = build_core_nodes(n_cpus);
    size_t n_smt = build_smt_nodes(n_cpus);
    size_t n_llc = build_llc_nodes(n_cores);
    size_t n_numa = build_numa_nodes(n_cores, n_llc);
    size_t n_packages = build_package_nodes(n_cores, n_llc);

    build_machine_node(n_packages);

    global.cpu_topology.level[TL_SMT] = smt_nodes;
    global.cpu_topology.count[TL_SMT] = n_smt;
    global.cpu_topology.level[TL_CORE] = core_nodes;
    global.cpu_topology.count[TL_CORE] = n_cores;
    global.cpu_topology.level[TL_NUMA] = numa_nodes;
    global.cpu_topology.count[TL_NUMA] = n_numa;
    global.cpu_topology.level[TL_LLC] = llc_nodes;
    global.cpu_topology.count[TL_LLC] = n_llc;
    global.cpu_topology.level[TL_PACKAGE] = package_nodes;
    global.cpu_topology.count[TL_PACKAGE] = n_packages;
    global.cpu_topology.level[TL_MACHINE] = &machine_node;
    global.cpu_topology.count[TL_MACHINE] = 1;

    topology_dump();
}
