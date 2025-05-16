#include <core.h>
#include <dbg.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <limine.h>
#include <mp.h>
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
#include <thread.h>
#include <vfs.h>
#include <vmalloc.h>
#include <vmm.h>

struct scheduler global_sched;
uint64_t t3_id = 0;

#define make_task(id, sauce, terminate)                                        \
    void task##id() {                                                          \
        while (1) {                                                            \
            k_printf("task %d says %s\n", id, sauce);                          \
            for (int i = 0; i < 50; i++)                                       \
                asm("hlt");                                                    \
            if (terminate)                                                     \
                scheduler_rm_id(&global_sched, t3_id);                         \
        }                                                                      \
    }
make_task(1, "MAYOOOO", true);
make_task(2, "MUSTAAARD", false);
make_task(3, "KETCHUUUP", false);
make_task(4, "RAAAANCH", false);
make_task(5, "SAUERKRAAAUUUT", false);

#define make_mp_task(id, os)                                                   \
    void task_mp##id() {                                                       \
        k_printf("multiprocessor task %d says %s\n", id, os);                  \
        while (1)                                                              \
            asm("cli;hlt");                                                    \
    }

make_mp_task(1, "linux");
make_mp_task(2, "macos");
make_mp_task(3, "freebsd");
make_mp_task(4, "openbsd");
make_mp_task(5, "solaris");

extern void test_alloc();

void kmain(void) {
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_mp_response *mpr = mp_request.response;

    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }

    enable_smap_smep_umip();
    gdt_install();
    idt_install();
    struct limine_hhdm_response *r = hhdm_request.response;
    init_physical_allocator(r->offset, memmap_request);
    vmm_offset_set(r->offset);
    vmm_init();
    vfs_init();
    read_test();
    test_alloc();
    core_data = vmm_alloc_pages(1);
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3_ready = 1;
    while (current_cpu != mpr->cpu_count - 1) {
        asm volatile("pause");
    }
    k_printf("Core %lu is available..\n", mp_available_core());
    global_sched.active = true;
    global_sched.started_first = false;
    struct thread *t1 = thread_create(task1);
    struct thread *t2 = thread_create(task2);
    struct thread *t3 = thread_create(task3);
    struct thread *t4 = thread_create(task4);
    struct thread *t5 = thread_create(task5);
    t3_id = t3->id;
    scheduler_init(&global_sched);
    scheduler_add_thread(&global_sched, t1);
    scheduler_add_thread(&global_sched, t2);
    scheduler_add_thread(&global_sched, t3);
    scheduler_add_thread(&global_sched, t4);
    scheduler_add_thread(&global_sched, t5);
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
