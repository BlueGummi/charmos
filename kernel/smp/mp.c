#include <acpi/lapic.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <smp/smp.h>
#include <sch/sched.h>
#include <sync/spinlock.h>

static uint64_t cr3 = 0;
static atomic_char cr3_ready = 0;
static struct spinlock wakeup_lock = SPINLOCK_INIT;
static atomic_uint cores_awake = 0;

static void detect_llc(struct topology_cache_info *llc) {
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
    c->core = c;
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

void smp_wakeup() {
    enum irql irql = spin_lock(&wakeup_lock);
    disable_interrupts();
    smap_init();
    serial_init();

    while (!cr3_ready)
        cpu_relax();

    asm volatile("mov %0, %%cr3" ::"r"(cr3));

    x2apic_init();

    uint64_t cpu = cpu_get_this_id();

    gdt_install();
    idt_load();

    setup_cpu(cpu);
    lapic_timer_init(cpu);
    set_core_awake();
    spin_unlock(&wakeup_lock, irql);

    enable_interrupts();
    scheduler_yield();
    while (1)
        wait_for_interrupt();
}

void smp_wakeup_processors(struct limine_mp_response *mpr) {
    for (uint64_t i = 0; i < mpr->cpu_count; i++)
        mpr->cpus[i]->goto_address = smp_wakeup;
}

void smp_complete_init() {
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    atomic_store(&cr3_ready, 1);
    if (global.core_count == 1)
        return;

    /* I know, I know, mmio is used here to force the read */
    while (mmio_read_8((uint8_t *) &global.current_bootstage) !=
           BOOTSTAGE_MID_MP)
        ;
}

void smp_setup_bsp() {
    struct core *c = kzalloc(sizeof(struct core));
    if (!c)
        k_panic("Could not allocate space for core structure on BSP");

    c->id = 0;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores = kmalloc(sizeof(struct core *) * global.core_count);

    if (unlikely(!global.cores))
        k_panic("Could not allocate space for global core structures");

    global.cores[0] = c;
    init_smt_info(c);
    detect_llc(&c->llc);
}
