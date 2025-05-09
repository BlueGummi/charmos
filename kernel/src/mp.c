#include <gdt.h>
#include <mp.h>
#include <printf.h>
#include <smap.h>
#include <spin_lock.h>
#include <vmalloc.h>

void wakeup() {
    spin_lock(&wakeup_lock);
    enable_smap_smep_umip();
    gdt_install();
    while (!cr3_ready)
        ;
    current_cpu++;
    k_printf("I am the %lu cpu\n", current_cpu);
    asm volatile("mov %0, %%cr3" ::"r"(cr3));
    int cpu = current_cpu;
    struct core *current_core = vmm_alloc_pages(1);
    current_core->id = cpu;
    current_core->state = IDLE;
    current_core->current_task = NULL;
    core_data[cpu] = current_core;
    spin_unlock(&wakeup_lock);

    while (1) {
        spin_lock(&wakeup_lock);
        spin_unlock(&wakeup_lock);
    }
}
