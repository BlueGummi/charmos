#include <acpi/lapic.h>
#include <asm.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
uint64_t *lapic;

void lapic_init(void) {
    uintptr_t lapic_phys = rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MASK;
    lapic = vmm_map_phys(lapic_phys, PAGE_SIZE, PAGING_UNCACHABLE);
}

void lapic_timer_init() {
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_SVR), LAPIC_ENABLE | 0xFF);

    LAPIC_SEND(LAPIC_REG(LAPIC_REG_TIMER_DIV), 0b0011);

    LAPIC_SEND(LAPIC_REG(LAPIC_REG_LVT_TIMER),
               TIMER_VECTOR | TIMER_MODE_PERIODIC);

    LAPIC_SEND(LAPIC_REG(LAPIC_REG_TIMER_INIT), 100000);
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

void lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    LAPIC_SEND(LAPIC_REG(LAPIC_ICR_HIGH), apic_id << LAPIC_DEST_SHIFT);
    LAPIC_SEND(LAPIC_REG(LAPIC_ICR_LOW), vector | LAPIC_DELIVERY_FIXED |
                                             LAPIC_LEVEL_ASSERT |
                                             LAPIC_DEST_PHYSICAL);
}

uint64_t lapic_get_id(void) {
    uint32_t lapic_id_raw = LAPIC_READ(LAPIC_REG(LAPIC_REG_ID));
    uint64_t cpu = (lapic_id_raw >> 24) & 0xFF;
    return cpu;
}
