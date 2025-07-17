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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spin_lock.h>

static uint64_t cr3 = 0;
static atomic_char cr3_ready = 0;
static struct spinlock wakeup_lock = SPINLOCK_INIT;
struct core **global_cores = NULL;
bool mp_ready = false;
static atomic_uint cores_awake = 0;

static void setup_cpu(uint64_t cpu) {
    struct core *c = kmalloc(sizeof(struct core));
    if (!c)
        k_panic("Core %d could not allocate space for struct\n", cpu);
    c->id = cpu;
    c->state = IDLE;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global_cores[cpu] = c;
}

static inline void set_core_awake(void) {
    atomic_fetch_add_explicit(&cores_awake, 1, memory_order_release);
    if (atomic_load_explicit(&cores_awake, memory_order_acquire) ==
        scheduler_get_core_count())
        mp_ready = true;
}

void wakeup() {
    bool ints = spin_lock(&wakeup_lock);
    smap_init();
    serial_init();

    while (!cr3_ready)
        cpu_relax();

    asm volatile("mov %0, %%cr3" ::"r"(cr3));

    uint64_t cpu = lapic_get_id();

    gdt_install();
    idt_install(cpu);
    lapic_timer_init();

    setup_cpu(cpu);
    spin_unlock(&wakeup_lock, ints);
    set_core_awake();

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

void mp_complete_init() {
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

    global_cores[0] = c;
}
