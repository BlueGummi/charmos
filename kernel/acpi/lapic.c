#include <acpi/lapic.h>
#include <asm.h>
#include <charmos.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <smp/core.h>
#include <sleep.h>
uint32_t *lapic;
bool x2apic_enabled = false;

void lapic_init(void) {
    uintptr_t lapic_phys = rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MASK;
    lapic = vmm_map_phys(lapic_phys, PAGE_SIZE, PAGING_UNCACHABLE);
}

void lapic_timer_init(uint64_t core_id) {
    uint32_t calibration_sleep_ms = 2;
    uint32_t timeslice_ms = 15;

    lapic_write(LAPIC_REG_SVR, LAPIC_ENABLE | 0xFF);
    lapic_write(LAPIC_REG_TIMER_DIV, 0b0011);
    lapic_write(LAPIC_REG_LVT_TIMER, TIMER_VECTOR | LAPIC_LVT_MASK);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

    sleep_ms(calibration_sleep_ms);

    uint32_t curr = lapic_read((LAPIC_REG_TIMER_CUR));
    uint32_t elapsed = 0xFFFFFFFF - curr;

    uint64_t lapic_calibrated_freq = elapsed * (1000 / calibration_sleep_ms);

    uint32_t timeslice_ticks = (lapic_calibrated_freq * timeslice_ms) / 1000;

    global.cores[core_id]->lapic_freq = lapic_calibrated_freq;
    lapic_write(LAPIC_REG_LVT_TIMER, TIMER_VECTOR | TIMER_MODE_PERIODIC);

    lapic_write(LAPIC_REG_TIMER_INIT, timeslice_ticks);
}

void lapic_timer_set_ms(uint32_t ms) {
    uint32_t ticks = (smp_core()->lapic_freq * ms) / 1000;

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

bool lapic_timer_is_enabled() {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    return !(lvt & (1 << 16));
}

static void lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << LAPIC_DEST_SHIFT);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_DELIVERY_FIXED |
                                   LAPIC_LEVEL_ASSERT | LAPIC_DEST_PHYSICAL);
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

void broadcast_nmi_except(uint64_t exclude_core) {
    for (uint64_t i = 0; i < global.core_count; i++) {
        if (i == exclude_core)
            continue;

        ipi_send(i, IRQ_PANIC);
    }
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
