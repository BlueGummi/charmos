#include <acpi/lapic.h>
#include <boot/gdt.h>
#include <crypto/prng.h>
#include <irq/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/tlb.h>
#include <sch/sched.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/smp.h>
#include <string.h>
#include <sync/spinlock.h>
#include <thread/dpc.h>
#include <time.h>

static volatile uint64_t cr3 = 0;
static _Atomic uint32_t cores_awake = 0;
#define CPUID_LEAF_HYBRID 0x1A

static void detect_cpu_features(struct cpu_capability *cap) {
    uint32_t eax, ebx, ecx, edx;

    cap->feature_bits = 0;

    /* CPUID.1 */
    cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);

    if (edx & (1 << 26))
        cap->feature_bits |= CPU_FEAT_SSE2;
    if (ecx & (1 << 28))
        cap->feature_bits |= CPU_FEAT_AVX;

    /* CPUID.7.0 */
    cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);

    if (ebx & (1 << 5))
        cap->feature_bits |= CPU_FEAT_AVX2;
    if (ebx & (1 << 16))
        cap->feature_bits |= CPU_FEAT_AVX512F;
}

static void detect_cpu_class(struct cpu_capability *cap) {
    uint32_t eax, ebx, ecx, edx;

    cpuid_count(CPUID_LEAF_HYBRID, 0, &eax, &ebx, &ecx, &edx);

    uint32_t core_type = (eax >> 24) & 0xFF;

    switch (core_type) {
    case 0x40: cap->class = CPU_CLASS_PERFORMANCE; break;
    case 0x20: cap->class = CPU_CLASS_EFFICIENCY; break;
    default: cap->class = CPU_CLASS_UNKNOWN; break;
    }

    cap->uarch_id = core_type;
}

static uint32_t detect_uarch_id(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);

    uint32_t family = (eax >> 8) & 0xF;
    uint32_t model = (eax >> 4) & 0xF;

    if (family == 6)
        model |= ((eax >> 16) & 0xF) << 4;

    switch (model) {
    case 0x97: /* Alder Lake P */
    case 0x9A: /* Raptor Lake P */ return UARCH_GOLDEN_COVE;
    case 0x9C: /* Alder Lake E */ return UARCH_GRACEMONT;
    default: return UARCH_UNKNOWN;
    }
}

static void detect_pipeline_width(struct cpu_capability *cap) {
    switch (cap->uarch_id) {
    case UARCH_GOLDEN_COVE:
        cap->issue_width = 6;
        cap->retire_width = 6;
        break;

    case UARCH_GRACEMONT:
        cap->issue_width = 3;
        cap->retire_width = 3;
        break;

    default:
        cap->issue_width = 2;
        cap->retire_width = 2;
        break;
    }
}

static const char *cpu_class_str(enum cpu_class c) {
    switch (c) {
    case CPU_CLASS_PERFORMANCE: return "P-core";
    case CPU_CLASS_EFFICIENCY: return "E-core";
    default: return "unknown";
    }
}

static const char *uarch_str(uint32_t uarch) {
    switch (uarch) {
    case UARCH_GOLDEN_COVE: return "Golden Cove";
    case UARCH_GRACEMONT: return "Gracemont";
    default: return "unknown";
    }
}

static void dump_cpu_features(uint64_t f) {
    char buf[128];
    buf[0] = '\0';

    if (f & CPU_FEAT_SSE2)
        strcat(buf, " SSE2");
    if (f & CPU_FEAT_AVX)
        strcat(buf, " AVX");
    if (f & CPU_FEAT_AVX2)
        strcat(buf, " AVX2");
    if (f & CPU_FEAT_AVX512F)
        strcat(buf, " AVX-512F");

    if (buf[0] == '\0')
        strcpy(buf, " (none)");

    log_msg(LOG_INFO, "    Features:%s", buf);
}

void smp_dump_core(struct core *c) {
    log_msg(LOG_INFO, "CPU%zu: pkg=%u core=%u smt=%u numa=%zu domain_cpu=%zu",
            c->id, c->package_id, c->core_id, c->smt_id, c->numa_node,
            c->domain_cpu_id);

    /* SMT / topology */
    log_msg(LOG_INFO, "  Topology: smt_mask=0x%x llc_shared=%u", c->smt_mask,
            c->llc.cores_sharing);

    /* Cache */
    log_msg(LOG_INFO, "  LLC: L%u size=%uKB line=%uB type=%u", c->llc.level,
            c->llc.size_kb, c->llc.line_size, c->llc.type);

    /* CPU capability block */
    struct cpu_capability *cap = &c->cap;

    log_msg(LOG_INFO, "  Class: %s (%s)", cpu_class_str(cap->class),
            uarch_str(cap->uarch_id));

    log_msg(LOG_INFO, "  Widths: issue=%u retire=%u", cap->issue_width,
            cap->retire_width);

    log_msg(LOG_INFO, "  Scores: perf=%u energy=%u", cap->perf_score,
            cap->energy_score);

    dump_cpu_features(cap->feature_bits);
}

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

static void detect_cpu_capability(struct core *c) {
    struct cpu_capability *cap = &c->cap;

    detect_cpu_features(cap);

    cap->uarch_id = detect_uarch_id();

    detect_cpu_class(cap);
    detect_pipeline_width(cap);
}

static struct core *setup_cpu(uint64_t cpu) {
    struct core *c =
        kzalloc(PAGE_ALIGN_UP(sizeof(struct core)), ALLOC_PARAMS_DEFAULT);
    if (!c)
        panic("Core %d could not allocate space for struct\n", cpu);
    c->id = cpu;
    c->self = c;
    c->current_irql = IRQL_PASSIVE_LEVEL;
    c->tsc_hz = tsc_calibrate();
    init_smt_info(c);
    detect_llc(&c->llc);
    detect_cpu_capability(c);

    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores[cpu] = c;
    return c;
}

static inline void set_core_awake(void) {
    atomic_fetch_add_explicit(&cores_awake, 1, memory_order_release);
    if (atomic_load_explicit(&cores_awake, memory_order_acquire) ==
        (global.core_count - 1)) {
        bootstage_advance(BOOTSTAGE_MID_MP);
    }
}

void smp_wakeup() {
    disable_interrupts();

    asm volatile("mov %0, %%cr3" ::"r"(cr3));

    x2apic_init();

    uint64_t cpu = cpu_get_this_id();

    gdt_install();
    irq_load();

    setup_cpu(cpu);
    lapic_timer_init(cpu);
    set_core_awake();

    scheduler_yield();
}

void smp_wait_for_others_to_idle() {
    /* wait for them to enter idle threads */
    size_t expected_idle = global.core_count - 1;
    while (atomic_load(&global.idle_core_count) < expected_idle) {
        cpu_relax();
    }
}

void smp_init(struct limine_mp_response *mpr) {
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    for (uint64_t i = 0; i < mpr->cpu_count; i++)
        mpr->cpus[i]->goto_address = smp_wakeup;

    smp_core()->tsc_hz = tsc_calibrate();
    if (global.core_count == 1)
        return;

    /* wait for bootstage to progress */
    while (global.current_bootstage != BOOTSTAGE_MID_MP)
        cpu_relax();

    smp_wait_for_others_to_idle();
}

void smp_setup_bsp() {
    struct core *c =
        kzalloc(PAGE_ALIGN_UP(sizeof(struct core)), ALLOC_PARAMS_DEFAULT);
    if (!c)
        panic("Could not allocate space for core structure on BSP");

    c->id = 0;
    c->self = c;
    c->current_irql = IRQL_PASSIVE_LEVEL;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores = kzalloc(sizeof(struct core *) * global.core_count,
                           ALLOC_PARAMS_DEFAULT);

    if (unlikely(!global.cores))
        panic("Could not allocate space for global core structures");

    global.shootdown_data =
        kzalloc(sizeof(struct tlb_shootdown_cpu) * global.core_count,
                ALLOC_PARAMS_DEFAULT);
    if (!global.shootdown_data)
        panic("Could not allocate global shootdown data\n");

    global.cores[0] = c;
    init_smt_info(c);
    detect_llc(&c->llc);
    detect_cpu_capability(c);
}

static atomic_uint tick_change_state = 0;
static bool enable = false;
static uint8_t entry = 0;

static enum irq_result tick_op_isr(void *ctx, uint8_t vector,
                                   struct irq_context *rsp) {
    if (enable) {
        scheduler_tick_enable();
    } else {
        scheduler_tick_disable();
    }

    atomic_fetch_add(&tick_change_state, 1);
    return IRQ_HANDLED;
}

static void send_em_all_out(bool e) {
    enable = e;

    atomic_store(&tick_change_state, 0);
    size_t i;
    for_each_cpu_id(i) {
        if (i == 0)
            continue;

        ipi_send(i, entry);
    }

    /* wait for everyone to change their tick state */
    while (atomic_load(&tick_change_state) < (global.core_count - 1))
        cpu_relax();
}

void smp_disable_all_ticks() {
    entry = irq_alloc_entry();
    irq_register("tick_op", entry, tick_op_isr, NULL, IRQ_FLAG_NONE);
    irq_set_chip(entry, lapic_get_chip(), NULL);
    send_em_all_out(false);
}

extern void nop_handler(void *, uint8_t, void *);
void smp_enable_all_ticks() {
    send_em_all_out(true);
    irq_free_entry(entry);
    irq_set_chip(entry, NULL, NULL);
}
