#include <acpi/lapic.h>
#include <asm.h>
#include <drivers/mmio.h>
#include <global.h>
#include <irq/idt.h>
#include <log.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <string.h>
#include <time/clock_evdev.h>
#include <time/names.h>
#include <time/spin_sleep.h>

uint32_t *lapic;
bool x2apic_enabled = false;
static LOG_HANDLE_DECLARE_DEFAULT(lapic);

void lapic_init(void) {
    uintptr_t lapic_phys = rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MASK;
    lapic = mmio_map(lapic_phys, PAGE_SIZE);
}

void lapic_timer_init(cpu_id_t core_id) {
    uint32_t calibration_sleep_ms = 2;

    lapic_write(LAPIC_REG_SVR, LAPIC_ENABLE | 0xFF);
    lapic_write(LAPIC_REG_TIMER_DIV, 0b0011);
    lapic_write(LAPIC_REG_LVT_TIMER, IRQ_TIMER | LAPIC_LVT_MASK);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

    sleep_spin_ms(calibration_sleep_ms);

    uint32_t curr = lapic_read((LAPIC_REG_TIMER_CUR));
    uint32_t elapsed = 0xFFFFFFFF - curr;

    freq_khz_t lapic_calibrated_khz = elapsed / calibration_sleep_ms;

    global.cores[core_id]->lapic_khz = lapic_calibrated_khz;
    lapic_timer_disable();
}

void lapic_timer_set_ms(uint32_t ms) {
    uint32_t ticks = smp_core()->lapic_khz * ms;

    lapic_write(LAPIC_REG_TIMER_INIT, ticks);
}

void lapic_timer_disable() {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    lvt |= LAPIC_LVT_MASK;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);
}

void lapic_timer_enable() {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    lvt &= ~LAPIC_LVT_MASK;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);
}

void lapic_eoi(struct irq_desc *unused) {
    (void) unused;
    lapic_write(LAPIC_REG_EOI, 0);
}

bool lapic_timer_is_enabled() {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    return !(lvt & (1 << 16));
}

static void lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_IPI_IN_FLIGHT)
        cpu_relax();

    lapic_write(LAPIC_ICR_HIGH, apic_id << LAPIC_DEST_SHIFT);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_DELIVERY_FIXED |
                                   LAPIC_LEVEL_ASSERT | LAPIC_DEST_PHYSICAL);
    irql_lower(irql);
}

void x2apic_send_ipi(uint32_t apic_id, uint8_t vector) {
    uint64_t icr = 0;
    icr |= vector;
    icr |= LAPIC_DELIVERY_FIXED;
    icr |= LAPIC_LEVEL_ASSERT;
    icr |= LAPIC_DEST_PHYSICAL;
    icr |= ((uint64_t) apic_id << 32);

    wrmsr(IA32_X2APIC_ICR, icr);
}

void panic_broadcast(uint64_t exclude_core) {
    size_t i;
    for_each_cpu_id(i) {
        if (i == exclude_core)
            continue;

        nmi_send(i);
    }
}

void nmi_send(uint32_t apic_id) {
    if (x2apic_enabled) {
        uint64_t icr = 0;

        /* Delivery mode = NMI. Vector ignored. */
        icr |= LAPIC_DELIVERY_NMI;  /* bits 10:8 = 100b for NMI */
        icr |= LAPIC_DEST_PHYSICAL; /* physical dest mode */
        icr |= ((uint64_t) apic_id << 32);

        wrmsr(IA32_X2APIC_ICR, icr);
        return;
    }

    uint32_t hi = apic_id << LAPIC_DEST_SHIFT;
    uint32_t lo = 0;

    lo |= LAPIC_DELIVERY_NMI;
    lo |= LAPIC_DEST_PHYSICAL;
    lo |= LAPIC_LEVEL_ASSERT;

    /* High must be written before low */
    lapic_write(LAPIC_ICR_HIGH, hi);
    lapic_write(LAPIC_ICR_LOW, lo);

    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_IPI_IN_FLIGHT)
        cpu_relax();
}

static int cpu_has_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;

    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1)
                 :);

    return (ecx >> 21) & 1;
}

void x2apic_init(void) {
    if (!cpu_has_x2apic())
        return;

    x2apic_enabled = true;
    uint64_t apic_base;
    apic_base = rdmsr(IA32_APIC_BASE);
    apic_base |= APIC_X2APIC_ENABLE;
    wrmsr(IA32_APIC_BASE, apic_base);
    log_info_global(LOG_HANDLE(lapic), "X2APIC enabled");
}

uint32_t x2apic_get_id(void) {
    return rdmsr(IA32_X2APIC_ID) & 0xFFFFFFFF;
}

uint64_t lapic_get_id(void) {
    uint32_t lapic_id_raw = lapic_read(LAPIC_REG_ID);
    uint64_t cpu = (lapic_id_raw >> 24) & 0xFF;
    return cpu;
}

uint32_t cpu_get_this_id(void) {
    return cpu_has_x2apic() ? x2apic_get_id() : lapic_get_id();
}

void ipi_send(uint32_t apic_id, uint8_t vector) {
    if (x2apic_enabled)
        return x2apic_send_ipi(apic_id, vector);

    lapic_send_ipi(apic_id, vector);
}

static struct irq_chip lapic_irq_chip = {
    .eoi = lapic_eoi,
    .mask = NULL,
    .unmask = NULL,
    .set_affinity = NULL,
    .set_rate_limit = NULL,
    .name = "lapic",
};

struct irq_chip *lapic_get_chip() {
    return &lapic_irq_chip;
}

void lapic_timer_init_bsp(void) {
    lapic_timer_init(0);
}

static enum errno lapic_evdev_set_next_event(struct clock_evdev *ced,
                                             time_ns_t delta_ns) {
    (void) ced;

    freq_khz_t freq_khz = smp_core()->lapic_khz;
    uint64_t ticks = (freq_khz * (uint64_t) delta_ns) / 1000000ULL;

    if (ticks == 0)
        ticks = 1;

    if (ticks > 0xFFFFFFFFULL)
        ticks = 0xFFFFFFFFULL;

    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    lvt &= ~(TIMER_MODE_PERIODIC | LAPIC_LVT_MASK);
    lvt |= (IRQ_TIMER | TIMER_MODE_ONESHOT);
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);

    /* Oneshot countdown */
    lapic_write(LAPIC_REG_TIMER_INIT, (uint32_t) ticks);

    return 0;
}

static enum errno lapic_evdev_change_state(struct clock_evdev *ced,
                                           enum clock_evdev_state state) {
    (void) ced;
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);

    switch (state) {
    case CLOCK_EVDEV_STATE_ONESHOT:
        lvt &= ~(TIMER_MODE_PERIODIC | LAPIC_LVT_MASK);
        lvt |= (IRQ_TIMER | TIMER_MODE_ONESHOT);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt);
        break;

    case CLOCK_EVDEV_STATE_PERIODIC:
        lvt &= ~LAPIC_LVT_MASK;
        lvt |= (IRQ_TIMER | TIMER_MODE_PERIODIC);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt);
        break;

    case CLOCK_EVDEV_STATE_OFF:
    case CLOCK_EVDEV_STATE_ONESHOT_STOPPED:
        lapic_write(LAPIC_REG_TIMER_INIT, 0);
        lvt |= LAPIC_LVT_MASK;
        lapic_write(LAPIC_REG_LVT_TIMER, lvt);
        break;
    }

    ced->state = state;
    return 0;
}

static struct clock_evdev *lapic_clock_evdev_create(cpu_id_t core_id) {
    struct clock_evdev *ced = clock_evdev_create("lapic_timer_%zu", core_id);

    ced->set_next_event = lapic_evdev_set_next_event;
    ced->change_state = lapic_evdev_change_state;

    /* bounds based on LAPIC frequency */
    freq_khz_t freq_khz = global.cores[core_id]->lapic_khz;

    ced->min_delta_ns = (1000000ULL + freq_khz - 1) / freq_khz;
    ced->max_delta_ns = (0xFFFFFFFFULL * 1000000ULL) / freq_khz;

    ced->min_delta_ticks = 1;
    ced->max_delta_ticks = 0xFFFFFFFF;

    ced->flags =
        CLOCK_EVDEV_ONESHOT | CLOCK_EVDEV_PERCPU | CLOCK_EVDEV_TICK_SUITABLE;
    ced->rating = CLOCK_RATING_BEST;
    ced->bound_to_cpu = core_id;
    ced->irq = IRQ_TIMER;

    /* ready LAPIC divider and vector */
    lapic_write(LAPIC_REG_TIMER_DIV, 0b0011);

    lapic_evdev_change_state(ced, CLOCK_EVDEV_STATE_ONESHOT_STOPPED);
    return ced;
}

void lapic_clock_evdev_group_init(void) {
    struct clock_evdev_group *cedg =
        alloc_or_die(clock_evdev_group_create(CLOCK_NAME_LAPIC));

    /* No need to set evdev_for_cpu if CLOCK_EVDEV_GROUP_PERCPU set */
    cedg->flags = CLOCK_EVDEV_GROUP_PERCPU;

    size_t i;
    for_each_cpu_id(i) {
        clock_evdev_group_add(cedg, lapic_clock_evdev_create(i));
    }

    clock_evdev_group_register(cedg);
}
