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
    c->smt_mask = (1 << smt_width) - 1;
    c->smt_id = apic_id & c->smt_mask;
    c->core_id = (apic_id >> smt_width) & ((1 << (core_width - smt_width)) - 1);
}

static void setup_cpu(uint64_t cpu) {
    struct core *c = kzalloc(sizeof(struct core));
    if (!c)
        k_panic("Core %d could not allocate space for struct\n", cpu);
    c->id = cpu;
    init_smt_info(c);
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores[cpu] = c;
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
    cr3_ready = true;
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
}

static struct topology_node *smt_nodes;
static struct topology_node *core_nodes;
static struct topology_node *numa_nodes;
static struct topology_node machine_node;

static void cpu_mask_print(const struct cpu_mask *m) {
    if (!m->uses_large) {
        k_printf("0x%llx", (unsigned long long) m->small);
    } else {
        size_t nwords = (m->nbits + 63) / 64;
        for (size_t i = 0; i < nwords; i++)
            k_printf("%016llx", (unsigned long long) m->large[nwords - 1 - i]);
    }
}

static void print_topology_node(struct topology_node *node, int depth) {
    for (int i = 0; i < depth; i++)
        k_printf("  ");

    const char *level_str = (node->level == TL_SMT)       ? "SMT"
                            : (node->level == TL_CORE)    ? "CORE"
                            : (node->level == TL_LLC)     ? "LLC"
                            : (node->level == TL_PACKAGE) ? "PKG"
                            : (node->level == TL_NUMA)    ? "NUMA"
                            : (node->level == TL_MACHINE) ? "MACH"
                                                          : "???";

    k_printf("[%s] id=%d parent=%d cpus=", level_str, node->id, node->parent);
    cpu_mask_print(&node->cpus);
    k_printf(" nr_children=%d\n", node->nr_children);

    if (node->level == TL_MACHINE) {
        for (int n = 0; n < global.cpu_topology.count[TL_NUMA]; n++)
            print_topology_node(&global.cpu_topology.level[TL_NUMA][n],
                                depth + 1);

    } else if (node->level == TL_NUMA) {
        for (int i = 0; i < global.cpu_topology.count[TL_CORE]; i++) {
            if (core_nodes[i].parent == node->id)
                print_topology_node(&core_nodes[i], depth + 1);
        }

    } else if (node->level == TL_CORE) {
        for (int i = 0; i < global.cpu_topology.count[TL_SMT]; i++) {
            if (smt_nodes[i].parent == node->id)
                print_topology_node(&smt_nodes[i], depth + 1);
        }
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
    print_topology_node(&machine_node, 0);
}

static size_t build_core_nodes(size_t n_cpus) {
    size_t core_count = 0;
    core_nodes = kzalloc(n_cpus * sizeof(struct topology_node));

    uint32_t *core_id_to_index = kzalloc(n_cpus * sizeof(uint32_t));
    memset(core_id_to_index, 0xFF, n_cpus * sizeof(uint32_t));

    for (size_t i = 0; i < n_cpus; i++) {
        struct core *c = global.cores[i];
        uint32_t core_id = c->core_id;

        if (core_id_to_index[core_id] == 0xFFFFFFFF) {
            struct topology_node *node = &core_nodes[core_count];
            node->level = TL_CORE;
            node->id = core_id;
            node->first_child = -1;
            node->nr_children = 0;
            node->core = c;

            cpu_mask_init(&node->cpus, n_cpus);
            cpu_mask_init(&node->idle, n_cpus);

            for (size_t j = 0; j < n_cpus; j++) {
                if (global.cores[j]->core_id == core_id) {
                    cpu_mask_set(&node->cpus, j);
                }
            }

            core_id_to_index[core_id] = core_count;
            core_count++;
        }
    }

    kfree(core_id_to_index);
    return core_count;
}

static size_t build_smt_nodes(size_t n_cpus) {
    smt_nodes = kzalloc(n_cpus * sizeof(struct topology_node));

    uint32_t *core_id_to_index = kzalloc(n_cpus * sizeof(uint32_t));
    memset(core_id_to_index, 0xFF, n_cpus * sizeof(uint32_t));

    for (size_t i = 0; i < n_cpus; i++) {
        for (size_t j = 0; j < n_cpus; j++) {
            if (core_nodes[j].id == global.cores[i]->core_id) {
                core_id_to_index[global.cores[i]->core_id] = j;
                break;
            }
        }
    }

    for (size_t i = 0; i < n_cpus; i++) {
        struct core *c = global.cores[i];
        struct topology_node *node = &smt_nodes[i];

        uint32_t core_index = core_id_to_index[c->core_id];

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
        if (parent_core->first_child == -1) {
            parent_core->first_child = i;
        }
        parent_core->nr_children++;
    }

    kfree(core_id_to_index);
    return n_cpus;
}

static size_t build_numa_nodes(size_t n_cores) {
    size_t max_numa = 0;
    for (size_t i = 0; i < n_cores; i++) {
        if (core_nodes[i].core->numa_node > max_numa) {
            max_numa = core_nodes[i].core->numa_node;
        }
    }

    size_t n_numa_nodes = max_numa + 1;
    numa_nodes = kzalloc(n_numa_nodes * sizeof(struct topology_node));

    for (size_t i = 0; i < n_numa_nodes; i++) {
        struct topology_node *numa = &numa_nodes[i];
        numa->level = TL_NUMA;
        numa->id = i;
        numa->parent = 0;
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
        if (numa->first_child == -1) {
            numa->first_child = i;
        }
        numa->nr_children++;
        cpu_mask_or(&numa->cpus, &numa->cpus, &core_nodes[i].cpus);
        cpu_mask_or(&numa->idle, &numa->idle, &core_nodes[i].idle);
    }

    return n_numa_nodes;
}

static void build_machine_node(size_t n_numa_nodes) {
    machine_node.level = TL_MACHINE;
    machine_node.id = 0;
    machine_node.parent = -1;
    machine_node.first_child = 0;
    machine_node.nr_children = n_numa_nodes;
    machine_node.core = NULL;

    cpu_mask_init(&machine_node.cpus, global.core_count);
    cpu_mask_init(&machine_node.idle, global.core_count);

    for (size_t i = 0; i < n_numa_nodes; i++) {
        numa_nodes[i].parent = 0;
        cpu_mask_or(&machine_node.cpus, &machine_node.cpus,
                    &numa_nodes[i].cpus);
        cpu_mask_or(&machine_node.idle, &machine_node.idle,
                    &numa_nodes[i].idle);
    }
}

void topology_init(void) {
    size_t n_cpus = global.core_count;

    size_t n_cores = build_core_nodes(n_cpus);
    size_t n_smt = build_smt_nodes(n_cpus);
    size_t n_numa = build_numa_nodes(n_cores);
    build_machine_node(n_numa);

    global.cpu_topology.level[TL_SMT] = smt_nodes;
    global.cpu_topology.count[TL_SMT] = n_smt;
    global.cpu_topology.level[TL_CORE] = core_nodes;
    global.cpu_topology.count[TL_CORE] = n_cores;
    global.cpu_topology.level[TL_NUMA] = numa_nodes;
    global.cpu_topology.count[TL_NUMA] = n_numa;
    global.cpu_topology.level[TL_PACKAGE] = &machine_node;
    global.cpu_topology.count[TL_PACKAGE] = 1;
    global.cpu_topology.level[TL_MACHINE] = &machine_node;
    global.cpu_topology.count[TL_MACHINE] = 1;

    topology_dump();
}
