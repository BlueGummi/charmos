#include <acpi/lapic.h>
#include <asm.h>
#include <cpuid_defs.h>
#include <drivers/mmio.h>
#include <irq/irq.h>
#include <log.h>
#include <mem/page.h>

uint32_t cc_mem_io *lapic;
bool x2apic_enabled = false;
LOG_HANDLE_DEFINE_PRINT_STATIC(lapic);

void lapic_init(void) {
    uintptr_t lapic_phys = rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MASK;
    lapic = mmio_map(lapic_phys, PAGE_SIZE);
}

static bool cpu_has_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid_count(CPUID_LEAF_FEATURES, 0, &eax, &ebx, &ecx, &edx);
    return ecx & CPUID_ECX_X2APIC;
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

uint32_t lapic_this_id(void) {
    return cpu_has_x2apic() ? x2apic_get_id() : lapic_get_id();
}

void lapic_eoi(struct irq_desc *unused) {
    cc_ignore(unused);
    lapic_write(LAPIC_REG_EOI, 0);
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

/* ===== ICR ===== */

static void x2apic_icr_write(uint32_t dest, uint32_t lo) {
    cpu_full_fence();
    wrmsr(IA32_X2APIC_ICR, ((uint64_t) dest << 32) | lo);
}

/* SDM forbids writing ICR whilst Delivery Status is Send Pending */
static bool xapic_icr_busy(void) {
    return lapic_read(LAPIC_ICR_LOW) & LAPIC_IPI_IN_FLIGHT;
}

/* High must be written before low, the low write sends */
static void xapic_icr_write(uint32_t dest, uint32_t lo) {
    lapic_write(LAPIC_ICR_HIGH, dest << LAPIC_DEST_SHIFT);
    lapic_write(LAPIC_ICR_LOW, lo);
}

/* IRQs off rather than an IRQL raise so the NMI/panic path can use this */
void lapic_icr_write(uint32_t dest, uint32_t lo) {
    if (x2apic_enabled)
        return x2apic_icr_write(dest, lo);

    bool iflag = irq_disable_save();
    while (xapic_icr_busy())
        cpu_pause();

    xapic_icr_write(dest, lo);
    irq_restore(iflag);
}

bool lapic_icr_try_write(uint32_t dest, uint32_t lo) {
    if (x2apic_enabled) {
        x2apic_icr_write(dest, lo);
        return true;
    }

    bool iflag = irq_disable_save();
    bool busy = xapic_icr_busy();
    if (!busy)
        xapic_icr_write(dest, lo);

    irq_restore(iflag);
    return !busy;
}

uint32_t lapic_icr_nmi_save(void) {
    if (x2apic_enabled || !lapic)
        return 0;

    return lapic_read(LAPIC_ICR_HIGH);
}

/* Wait on the ICR */
void lapic_icr_nmi_restore(uint32_t high) {
    if (x2apic_enabled || !lapic)
        return;

    while (xapic_icr_busy())
        cpu_pause();

    lapic_write(LAPIC_ICR_HIGH, high);
}
