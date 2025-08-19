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
#include <sync/spin_lock.h>

static uint64_t cr3 = 0;
static atomic_char cr3_ready = 0;
static struct spinlock wakeup_lock = SPINLOCK_INIT;
static atomic_uint cores_awake = 0;

static void init_smt_info(struct core *c) {

    uint32_t eax, ebx, ecx, edx;
    uint32_t apic_id = c->id;
    uint32_t smt_width = 0;
    uint32_t smt_mask = 0;

    for (uint32_t level = 0;; level++) {
        cpuid_count(0xB, level, &eax, &ebx, &ecx, &edx);
        uint32_t level_type = (ecx >> 8) & 0xFF;
        if (level_type == 0)
            break;
        if (level_type != 1)
            continue;

        smt_width = ebx & 0xFF;
        smt_mask = (1 << smt_width) - 1;

        c->smt_mask = smt_mask;
        c->smt_id = apic_id & smt_mask;
        break;
    }
}

static void setup_cpu(uint64_t cpu) {
    struct core *c = kzalloc(sizeof(struct core));
    if (!c)
        k_panic("Core %d could not allocate space for struct\n", cpu);
    init_smt_info(c);
    c->id = cpu;
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

static void print_topology_node(struct topology_node *node, int depth) {
    for (int i = 0; i < depth; i++)
        k_printf("  ");

    k_printf("[%s] id=%d parent=%d cpus=0x%llx nr_children=%d\n",
             (node->level == TL_SMT)       ? "SMT"
             : (node->level == TL_CORE)    ? "CORE"
             : (node->level == TL_LLC)     ? "LLC"
             : (node->level == TL_PACKAGE) ? "PKG"
             : (node->level == TL_NUMA)    ? "NUMA"
             : (node->level == TL_MACHINE) ? "MACH"
                                           : "???",
             node->id, node->parent, (unsigned long long) node->cpus,
             node->nr_children);

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
            if (smt_nodes[i].parent == core_nodes[i].id)

                print_topology_node(&smt_nodes[i], depth + 1);
        }
    }
}

void topology_dump(void) {
    print_topology_node(&machine_node, 0);
}

void topology_init(void) {
    uint64_t n_cpus = global.core_count;

    smt_nodes = kzalloc(sizeof(struct topology_node) * n_cpus);
    core_nodes = kzalloc(sizeof(struct topology_node) * n_cpus);

    for (uint64_t i = 0; i < n_cpus; i++) {
        struct core *c = global.cores[i];

        smt_nodes[i].level = TL_SMT;
        smt_nodes[i].id = c->id;

        smt_nodes[i].parent = i;
        smt_nodes[i].first_child = -1;
        smt_nodes[i].nr_children = 0;
        smt_nodes[i].cpus = 1ULL << c->id;
        smt_nodes[i].idle = smt_nodes[i].cpus;
        smt_nodes[i].core = c;

        core_nodes[i].level = TL_CORE;
        core_nodes[i].id = i;
        core_nodes[i].parent = 0;
        core_nodes[i].first_child = i;
        core_nodes[i].nr_children = 1;
        core_nodes[i].cpus = smt_nodes[i].cpus;
        core_nodes[i].idle = core_nodes[i].cpus;
        core_nodes[i].core = c;
    }

    machine_node.level = TL_MACHINE;
    machine_node.id = 0;
    machine_node.parent = -1;
    machine_node.first_child = 0;
    machine_node.nr_children = n_cpus;
    machine_node.cpus = 0;

    for (uint64_t i = 0; i < n_cpus; i++)
        machine_node.cpus |= core_nodes[i].cpus;
    machine_node.idle = machine_node.cpus;

    machine_node.core = NULL;

    global.cpu_topology.level[TL_SMT] = smt_nodes;

    global.cpu_topology.count[TL_SMT] = n_cpus;

    global.cpu_topology.level[TL_CORE] = core_nodes;
    global.cpu_topology.count[TL_CORE] = n_cpus;

    global.cpu_topology.level[TL_PACKAGE] = &machine_node;
    global.cpu_topology.count[TL_PACKAGE] = 1;

    global.cpu_topology.level[TL_MACHINE] = &machine_node;
    global.cpu_topology.count[TL_MACHINE] = 1;

    size_t max_numa_id = 0;
    for (uint64_t i = 0; i < n_cpus; i++)
        if (global.cores[i]->numa_node > max_numa_id)
            max_numa_id = global.cores[i]->numa_node;

    size_t n_numa_nodes = max_numa_id + 1;
    numa_nodes = kzalloc(sizeof(struct topology_node) * n_numa_nodes);

    for (size_t n = 0; n < n_numa_nodes; n++) {
        numa_nodes[n].level = TL_NUMA;
        numa_nodes[n].id = n;
        numa_nodes[n].parent = 0;
        numa_nodes[n].first_child = -1;
        numa_nodes[n].nr_children = 0;
        numa_nodes[n].cpus = 0;
        numa_nodes[n].idle = 0;
        numa_nodes[n].core = NULL;
    }

    for (uint64_t i = 0; i < n_cpus; i++) {

        struct core *c = global.cores[i];
        struct topology_node *numa = &numa_nodes[c->numa_node];

        if (numa->first_child == -1)
            numa->first_child = c->id;

        numa->nr_children++;
        numa->cpus |= 1ULL << c->id;
        numa->idle |= 1ULL << c->id;

        core_nodes[i].parent = numa->id;
    }

    machine_node.first_child = 0;
    machine_node.nr_children = n_numa_nodes;

    machine_node.cpus = 0;
    for (size_t n = 0; n < n_numa_nodes; n++)
        machine_node.cpus |= numa_nodes[n].cpus;

    global.cpu_topology.level[TL_NUMA] = numa_nodes;
    global.cpu_topology.count[TL_NUMA] = n_numa_nodes;
    topology_dump();
}
