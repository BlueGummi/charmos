#include <gdt.h>
#include <mp.h>
#include <printf.h>
#include <sched.h>
#include <smap.h>
#include <spin_lock.h>
#include <vmalloc.h>

struct core **core_data = NULL;
uint64_t cr3 = 0;
atomic_char cr3_ready = 0;
struct spinlock wakeup_lock = SPINLOCK_INIT;
atomic_uint_fast64_t current_cpu = 0;

/*
 * Return an available core # that is idle
 */
uint64_t mp_available_core() {
    int i = 1;
    while (core_data[i] != NULL) {
        if (core_data[i]->state == IDLE &&
            core_data[i]->current_thread == NULL) {
            return i;
        }
        i++;
    }
    return -1;
}

void wakeup() {
    spin_lock(&wakeup_lock);
    enable_smap_smep_umip();
    gdt_install();
    serial_init();
    while (!cr3_ready)
        ;
    current_cpu++;
    asm volatile("mov %0, %%cr3" ::"r"(cr3));
    int cpu = current_cpu;
    struct core *current_core = vmm_alloc_pages(1);
    current_core->id = cpu;
    current_core->state = IDLE;
    current_core->current_thread = NULL;
    core_data[cpu] = current_core;
    k_printf("Core %d waking up...\n", cpu);
    spin_unlock(&wakeup_lock);

    while (1) {
        spin_lock(&wakeup_lock);
        if (current_core->current_thread != NULL) {
            current_core->state = BUSY;
            spin_unlock(&wakeup_lock);
            current_core->current_thread->entry();
            thread_free(current_core->current_thread);
            current_core->current_thread = NULL;
            current_core->state = IDLE;
        }
        spin_unlock(&wakeup_lock);
    }
}
