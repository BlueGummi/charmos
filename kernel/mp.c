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
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

uint64_t cr3 = 0;
atomic_char cr3_ready = 0;
struct spinlock wakeup_lock = SPINLOCK_INIT;
uint64_t *lapic;
struct core **global_cores = NULL;
bool mp_ready = false;
static atomic_uint cores_awake = 0;

void wakeup() {
    bool ints = spin_lock(&wakeup_lock);
    smap_init();
    serial_init();
    while (!cr3_ready)
        cpu_relax();
    asm volatile("mov %0, %%cr3" ::"r"(cr3));

    uint32_t lapic_id_raw = LAPIC_READ(LAPIC_REG(LAPIC_REG_ID));
    uint64_t cpu = (lapic_id_raw >> 24) & 0xFF;
    struct core *c = kmalloc(sizeof(struct core));
    if (!c)
        k_panic("Core %d could not allocate space for struct\n", cpu);

    gdt_install();
    idt_install(cpu);
    lapic_init();
    c->id = cpu;
    c->state = IDLE;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global_cores[cpu] = c;

    spin_unlock(&wakeup_lock, ints);
    atomic_fetch_add_explicit(&cores_awake, 1, memory_order_release);

    if (atomic_load_explicit(&cores_awake, memory_order_acquire) ==
        scheduler_get_core_count())
        mp_ready = true;

    restore_interrupts();
    scheduler_yield();
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

void mp_setup_bsp(uint64_t core_count) {
    struct core *c = kmalloc(sizeof(struct core));
    if (!c)
        k_panic("Could not allocate space for core structure on BSP");

    c->state = IDLE;
    c->current_thread = kzalloc(sizeof(struct thread));
    if (unlikely(!c->current_thread))
        k_panic("Could not allocate space for BSP's current thread");

    c->id = 0;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global_cores = kmalloc(sizeof(struct core *) * core_count);
    if (unlikely(!global_cores))
        k_panic("Could not allocate space for global core structures");
}
