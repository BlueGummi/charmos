// #include "uacpi/internal/log.h"
#include "sched.h"
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
#include <shutdown.h>
#include <slock.h>
#include <smap.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <task.h>
#include <vfs.h>
#include <vmalloc.h>
#include <vmm.h>

struct spinlock wakeup_lock = SPINLOCK_INIT;
struct spinlock cpu_id_lock = SPINLOCK_INIT;
volatile uint32_t cpus_woken = 0;
int glob_cpu_c = 0;
uint64_t t1_id;
volatile uint32_t expected_cpu_id = 0;
struct scheduler global_sched;
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
void wakeup() {
    uint32_t my_cpu_id;

    spinlock_lock(&cpu_id_lock);
    my_cpu_id = glob_cpu_c++;
    spinlock_unlock(&cpu_id_lock);

    while (expected_cpu_id != my_cpu_id)
        asm volatile("pause");

    spinlock_lock(&wakeup_lock);
    switch (my_cpu_id) {
    case 0: k_printf("CPU %d says: bing bop\n", my_cpu_id + 1); break;
    case 1: k_printf("CPU %d says: boom boom\n", my_cpu_id + 1); break;
    case 2: k_printf("CPU %d says: boom bop\n", my_cpu_id + 1); break;
    }
    cpus_woken++;
    expected_cpu_id++;
    spinlock_unlock(&wakeup_lock);

    while (1)
        asm("hlt");
}

void kmain(void) {
    asm volatile("cli");
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        asm("hlt");
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        asm("hlt");
    }

    struct limine_framebuffer *fb =
        framebuffer_request.response->framebuffers[0];
    struct flanterm_context *ft_ctx = flanterm_fb_init(
        NULL, NULL, fb->address, fb->width, fb->height, fb->pitch,
        fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
        fb->green_mask_shift, fb->blue_mask_size, fb->blue_mask_shift, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 1, 0, 0, 0);
    k_printf_init(ft_ctx);

    struct limine_mp_response *mpr = mp_request.response;

    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }

    while (cpus_woken < mpr->cpu_count - 1) {
        asm volatile("pause");
    }

    enable_smap_smep_umip();
    gdt_install();
    init_interrupts();
    struct limine_hhdm_response *response = hhdm_request.response;
    init_physical_allocator(response->offset, memmap_request);
    vmm_offset_set(response->offset);
    vmm_init();
    extern uint8_t read_cmos(uint8_t reg);
    /*k_info((read_cmos(0x0) & 1) == 1
               ? "Houston, Tranquility Base here. The Eagle has landed."
               : "If puns were deli meat, this would be the wurst.");*/
    vfs_init();
    read_test();
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
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
