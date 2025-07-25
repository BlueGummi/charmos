#include <acpi/lapic.h>
#include <asm.h>
#include <charmos.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <mp/core.h>
#include <sleep.h>
uint32_t *lapic;

void lapic_init(void) {
    uintptr_t lapic_phys = rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MASK;
    lapic = vmm_map_phys(lapic_phys, PAGE_SIZE, PAGING_UNCACHABLE);
}

static uint32_t lapic_calibrated_freq = 0;

void lapic_timer_init() {
    uint32_t calibration_sleep_ms = 2;
    uint32_t timeslice_ms = 15;

    LAPIC_SEND(LAPIC_REG(LAPIC_REG_SVR), LAPIC_ENABLE | 0xFF);
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_TIMER_DIV), 0b0011);
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_LVT_TIMER), TIMER_VECTOR | LAPIC_LVT_MASK);
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_TIMER_INIT), 0xFFFFFFFF);

    sleep_ms(calibration_sleep_ms);

    uint32_t curr = LAPIC_READ(LAPIC_REG(LAPIC_REG_TIMER_CUR));
    uint32_t elapsed = 0xFFFFFFFF - curr;

    lapic_calibrated_freq = elapsed * (1000 / calibration_sleep_ms);

    uint32_t timeslice_ticks = (lapic_calibrated_freq * timeslice_ms) / 1000;

    LAPIC_SEND(LAPIC_REG(LAPIC_REG_LVT_TIMER),
               TIMER_VECTOR | TIMER_MODE_PERIODIC);

    LAPIC_SEND(LAPIC_REG(LAPIC_REG_TIMER_INIT), timeslice_ticks);
}

void lapic_timer_disable() {
    uint32_t lvt = LAPIC_READ(LAPIC_REG(LAPIC_REG_LVT_TIMER));
    lvt |= LAPIC_LVT_MASK;
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_LVT_TIMER), lvt);
}

void lapic_timer_enable() {
    uint32_t lvt = LAPIC_READ(LAPIC_REG(LAPIC_REG_LVT_TIMER));
    lvt &= ~LAPIC_LVT_MASK;
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_LVT_TIMER), lvt);
}

bool lapic_timer_is_enabled() {
    uint32_t lvt = LAPIC_READ(LAPIC_REG(LAPIC_REG_LVT_TIMER));
    return !(lvt & (1 << 16));
}

void lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    LAPIC_SEND(LAPIC_REG(LAPIC_ICR_HIGH), apic_id << LAPIC_DEST_SHIFT);
    LAPIC_SEND(LAPIC_REG(LAPIC_ICR_LOW), vector | LAPIC_DELIVERY_FIXED |
                                             LAPIC_LEVEL_ASSERT |
                                             LAPIC_DEST_PHYSICAL);
}

void broadcast_nmi_except(uint64_t exclude_core) {
    for (uint64_t i = 0; i < global.core_count; i++) {
        if (i == exclude_core)
            continue;

        lapic_send_ipi(i, IRQ_PANIC);
    }
}

uint64_t lapic_get_id(void) {
    uint32_t lapic_id_raw = LAPIC_READ(LAPIC_REG(LAPIC_REG_ID));
    uint64_t cpu = (lapic_id_raw >> 24) & 0xFF;
    return cpu;
}
