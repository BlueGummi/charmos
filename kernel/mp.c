#include <acpi/lapic.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <spin_lock.h>

uint64_t cr3 = 0;
atomic_char cr3_ready = 0;
struct spinlock wakeup_lock = SPINLOCK_INIT;
uint64_t *lapic;

void lapic_init() {
    LAPIC_REG(LAPIC_REG_SVR) = LAPIC_ENABLE | 0xFF;

    LAPIC_REG(LAPIC_REG_TIMER_DIV) = 0b0011;

    LAPIC_REG(LAPIC_REG_LVT_TIMER) = TIMER_VECTOR | TIMER_MODE_PERIODIC;

    LAPIC_REG(LAPIC_REG_TIMER_INIT) = 1000000;
}

void wakeup() {
    bool ints = spin_lock(&wakeup_lock);
    smap_init();
    gdt_install();
    serial_init();
    while (!cr3_ready)
        ;
    asm volatile("mov %0, %%cr3" ::"r"(cr3));

    uint32_t lapic_id_raw = LAPIC_REG(LAPIC_REG_ID);
    uint64_t cpu = (lapic_id_raw >> 24) & 0xFF;
    lapic_init();
    idt_install(cpu);

    struct core *c = kmalloc(sizeof(struct core));
    if (!c)
        k_panic("Core %d could not allocate space for struct\n", cpu);
    c->id = cpu;
    c->state = IDLE;
    wrmsr(MSR_GS_BASE, (uint64_t) c);

    spin_unlock(&wakeup_lock, ints);
    asm("sti");
    scheduler_start(local_schs[cpu]);

    while (1)
        asm("hlt");
}

void mp_wakeup_processors(struct limine_mp_response *mpr) {
    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }
}

void mp_inform_of_cr3() {
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3_ready = true;
}

uint64_t get_sch_core_id() {
    struct core *c = (struct core *) rdmsr(MSR_GS_BASE);
    return c->id;
}
