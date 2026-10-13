#include <acpi/lapic.h>
#include <boot/gdt.h>
#include <crypto/prng.h>
#include <irq/idt.h>
#include <irq/ipi.h>
#include <limine.h>
#include <math/bit.h>
#include <mem/alloc.h>
#include <mem/domain.h>
#include <mem/nofail.h>
#include <mem/tlb.h>
#include <sch/sched.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/smp.h>
#include <string.h>
#include <sync/once_latch.h>
#include <sync/spinlock.h>
#include <thread/dpc.h>
#include <thread/thread.h>
#include <time/lapic_timer.h>
#include <time/time.h>
#include <time/tsc.h>

#include "mem/tlb_internal.h" /* for tlb_shootdown_cpu */

/* TODO: This file is one that I had thought would stay small back when
 * I first got SMP working, but day by day it grows and grows. Might
 * want to refactor and clean this up into state machines */

static atomic_uint64_t cr3 = 0;
static atomic_uint32_t cores_awake = 0;

/* NOTE: this is used because it is contiguous in memory,
 * whereas percpu is NOT.
 *
 * TODO: implement PERCPU flags so that we can
 * initialize in a contiguous manner, and avoid this hassle
 * down the line. */
static struct dpc timer_init_dpcs[CPU_MASK_BITS];

static void timer_init_fanout_fn(void) {
    timers_init_ap(smp_id_raw());
}
#define CPUID_LEAF_HYBRID 0x1A

static void detect_cpu_features(struct cpu_capability *cap) {
    uint32_t eax, ebx, ecx, edx;

    cap->feature_bits = 0;

    /* CPUID.1 */
    cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);

    if (BIT_TEST(edx, 26))
        cap->feature_bits |= CPU_FEAT_SSE2;
    if (BIT_TEST(ecx, 28))
        cap->feature_bits |= CPU_FEAT_AVX;

    /* CPUID.7.0 */
    cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);

    if (BIT_TEST(ebx, 5))
        cap->feature_bits |= CPU_FEAT_AVX2;
    if (BIT_TEST(ebx, 16))
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
    c->smt_mask = BIT(smt_width) - 1;
    c->smt_id = apic_id & c->smt_mask;
    c->core_id = (apic_id >> smt_width) & (BIT(core_width - smt_width) - 1);
}

static void detect_cpu_capability(struct core *c) {
    struct cpu_capability *cap = &c->cap;

    detect_cpu_features(cap);

    cap->uarch_id = detect_uarch_id();

    detect_cpu_class(cap);
    detect_pipeline_width(cap);
}

static struct core *setup_cpu(uint64_t cpu) {
    struct core *c = global.cores[cpu];
    enforce(c->id == cpu);
    c->self = c;
    c->current_irql = IRQL_PASSIVE_LEVEL;
    c->tsc_hz = tsc_calibrate();
    init_smt_info(c);
    detect_llc(&c->llc);
    detect_cpu_capability(c);

    wrmsr(MSR_GS_BASE, (uint64_t) c);
    return c;
}

static inline void set_core_awake(void) {
    if (atomic_inc_return_acq_rel(&cores_awake) == (global.core_count - 1)) {
        bootstage_advance(BOOTSTAGE_MID_MP);
    }
}

/* returns with interrupts disabled; the scheduler takes it from here */
void smp_ap_wakeup(struct limine_mp_info *info) TSA_ACQUIRES_IRQS {
    cc_unused(info);
    irq_disable();

    cr3_write(atomic_load_acq(&cr3));

    x2apic_init();
    cpu_id_t cpu = smp_cpu_from_apic_id(lapic_this_id());
    setup_cpu(cpu);

    gdt_load();
    wrmsr(MSR_GS_BASE, (uint64_t) global.cores[cpu]);
    idt_load();

    lapic_timer_init(cpu);
    tsc_sync_check_ap(cpu);
    set_core_awake();

    scheduler_yield();
}

void smp_init(void) {
    for (size_t i = 0; i < global.core_count; i++) {
        size_t d = domain_for_cpu(i);

        if (i != 0) {
            global.cores[i] =
                nofail(valloc_from_domain(d, sizeof(struct core)));

            memset(global.cores[i], 0, sizeof(struct core));
        }

        global.cores[i]->id = i;
        global.cores[i]->numa_node = numa_node_for_cpu(i);
        global.cores[i]->domain = global.domains[d];
    }
}

void smp_timer_init(void) {
    struct cpu_mask all = CPU_MASK_INIT;
    cpu_mask_set_all(&all);

    dpc_fanout(timer_init_dpcs, all, timer_init_fanout_fn);
}

void smp_wait_for_others_to_idle(void) {
    /* wait for them to enter idle threads */
    size_t expected_idle = global.core_count - 1;
    while (atomic_load(&global.idle_core_count) < expected_idle) {
        cpu_pause();
    }
}

void smp_wake(struct limine_mp_response *mpr) {
    atomic_store_release(&cr3, cr3_read());
    for (uint64_t i = 1; i < mpr->cpu_count; i++)
        mpr->cpus[i]->goto_address = smp_ap_wakeup;

    smp_core(TOPC_NONE)->tsc_hz = tsc_calibrate();
    if (global.core_count == 1)
        return;

    tsc_sync_check_all_aps();
    /* wait for bootstage to progress, TODO: Find a better
     * way to do this, reading a volatile and NOT using
     * atomics here is problematic */
    while (global.current_bootstage != BOOTSTAGE_MID_MP)
        cpu_pause();
}

void smp_setup_bsp(void) {
    struct core *c = nofail_vnew(c, ALLOC_ZERO);

    c->id = 0;
    c->self = c;
    c->current_irql = IRQL_PASSIVE_LEVEL;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global.cores = nofail_vnews(global.cores, global.core_count, ALLOC_ZERO);

    global.cores[0] = c;
    init_smt_info(c);
    detect_llc(&c->llc);
    detect_cpu_capability(c);
}

static atomic_uint32_t tick_change_state = 0;
static atomic_bool enable = false;
static uint8_t entry = 0;

static enum irq_result tick_op_isr(void *ctx, uint8_t vector,
                                   struct irq_context *rsp) {
    cc_unused(ctx, vector, rsp);
    if (atomic_load_acq(&enable)) {
        scheduler_tick_enable();
    } else {
        scheduler_tick_disable();
    }

    atomic_inc(&tick_change_state);
    return IRQ_HANDLED;
}

static void send_em_all_out(bool e) {
    atomic_store_release(&enable, e);

    atomic_store(&tick_change_state, 0);
    size_t i;
    for_each_cpu_id(i) {
        if (i == 0)
            continue;

        ipi_send(i, entry);
    }

    /* wait for everyone to change their tick state */
    while (atomic_load(&tick_change_state) < (global.core_count - 1))
        cpu_pause();
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

struct core *smp_bsp(void) {
    return global.cores[0];
}

void smp_caller_verify(enum topology_caller caller) {
    struct topology_contract tct = {
        .caller = caller,
        .scope = TOPOLOGY_LEVEL_SMT,
    };

    bool valid = topology_contract_verify(tct);
    enforce(valid, "Caller 0x%x was not satisfied", caller);
}
