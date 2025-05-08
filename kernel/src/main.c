#include <dbg.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <limine.h>
#include <pmm.h>
#include <printf.h>
#include <requests.h>
#include <sched.h>
#include <shutdown.h>
#include <smap.h>
#include <spin_lock.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <task.h>
#include <vfs.h>
#include <vmalloc.h>
#include <vmm.h>

struct spinlock wakeup_lock = SPINLOCK_INIT;
int glob_cpu_c = 0;
uint64_t t1_id;
struct scheduler global_sched;
atomic_char do_work = 0;
atomic_uint_fast64_t current_cpu = 0;
#define make_task(id, sauce, terminate)                                        \
    void task##id() {                                                          \
        while (1) {                                                            \
            k_printf("task %d says %s\n", id, sauce);                          \
            for (int i = 0; i < 50; i++)                                       \
                asm("hlt");                                                    \
            if (terminate)                                                     \
                scheduler_remove_task_by_id(&global_sched, 3);                 \
        }                                                                      \
    }
make_task(1, "MAYOOOO", true);
make_task(2, "MUSTAAARD", false);
make_task(3, "KETCHUUUP", false);
make_task(4, "RAAAANCH", false);
make_task(5, "SAUERKRAAAUUUT", false);
extern void test_alloc();
void sad_loop() {
    while (1) {
        asm("hlt");
    }
}

void wakeup() {
    spin_lock(&wakeup_lock);
    enable_smap_smep_umip();
    gdt_install();
    current_cpu++;
    k_printf("I am the %lu cpu\n", current_cpu);
    int cpu = current_cpu;
    spin_unlock(&wakeup_lock);

    while (1) {
        spin_lock(&wakeup_lock);
        if (do_work) {
            k_printf("CPU %d says: I need to do work!\n", cpu);
            do_work = 0;
        }
        spin_unlock(&wakeup_lock);
    }
}

void kmain(void) {
    k_printf_init(framebuffer_request.response->framebuffers[0]);

    struct limine_mp_response *mpr = mp_request.response;

    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }

    while (current_cpu != mpr->cpu_count - 1) {
        asm volatile("pause");
    }
    k_printf("We have woken up everybody\n");

    enable_smap_smep_umip();
    gdt_install();
    init_interrupts();
    struct limine_hhdm_response *r = hhdm_request.response;
    init_physical_allocator(r->offset, memmap_request);
    vmm_offset_set(r->offset);
    vmm_init();
    vfs_init();
    read_test();
    test_alloc();
    global_sched.active = true;
    global_sched.started_first = false;
    struct task *t1 = create_task(task1);
    struct task *t2 = create_task(task2);
    struct task *t3 = create_task(task3);
    struct task *t4 = create_task(task4);
    struct task *t5 = create_task(task5);
    scheduler_init(&global_sched);
    scheduler_add_task(&global_sched, t1);
    scheduler_add_task(&global_sched, t2);
    scheduler_add_task(&global_sched, t3);
    scheduler_add_task(&global_sched, t4);
    scheduler_add_task(&global_sched, t5);
    do_work = 1;
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
