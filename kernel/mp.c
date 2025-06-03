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

struct core *core_data = NULL;
uint64_t cr3 = 0;
atomic_char cr3_ready = 0;
struct spinlock wakeup_lock = SPINLOCK_INIT;
uint64_t total_cpu = 0;
/*
 * Return an available core # that is idle
 */
uint64_t mp_available_core() {
    uint64_t i = 1;
    while (&core_data[i] != NULL) {
        if (core_data[i].state == IDLE && core_data[i].current_thread == NULL) {
            return i;
        }
        i++;
    }
    return (uint64_t) -1;
}

void wakeup() {
    bool ints = spin_lock(&wakeup_lock);
    enable_smap_smep_umip();
    gdt_install();
    serial_init();
    while (!cr3_ready)
        ;
    asm volatile("mov %0, %%cr3" ::"r"(cr3));
    uint64_t cpu = get_core_id();
    idt_install(cpu);
    struct core *c = kmalloc(sizeof(struct core));
    c->id = cpu;
    c->state = IDLE;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    k_printf("processor %d initialized\n", cpu);
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
    total_cpu = mpr->cpu_count;
}

void mp_inform_of_cr3() {
    core_data = kmalloc(sizeof(struct core) * total_cpu);
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3_ready = true;
}
