// #include "uacpi/internal/log.h"
#include <dbg.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <limine.h>
#include <memfuncs.h>
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
#include <task.h>
#include <vmalloc.h>
#include <vmm.h>

spinlock_t wakeup_lock = SPINLOCK_INIT;
spinlock_t cpu_id_lock = SPINLOCK_INIT;
volatile uint32_t cpus_woken = 0;
int glob_cpu_c = 0;
volatile uint32_t expected_cpu_id = 0;
struct task_t *current_task = NULL;
struct task_t *first_task = NULL;

#define make_task(id, sauce)                                                   \
    void task##id() {                                                          \
        while (1) {                                                            \
            k_printf("task %d says %s\n", id, sauce);                          \
            for (int i = 0; i < 10; i++)                                       \
                asm("hlt");                                                    \
            for (int i = 0; i < 50; i++) {                                     \
                asm("hlt");                                                    \
                k_printf("task %d runs the %dth loop!\n", id, i);              \
            }                                                                  \
        }                                                                      \
    }
make_task(1, "MAYOOOO");
make_task(2, "MUSTAAARD");
make_task(3, "KETCHUUUP");
make_task(4, "RAAAANCH");
make_task(5, "SAUERKRAAAUUUT");

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
    k_info("Framebuffer initialized");

    struct limine_mp_response *mpr = mp_request.response;

    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }

    while (cpus_woken < mpr->cpu_count - 1) {
        asm volatile("pause");
    }

    k_info("bam!");
    debug_print_stack();
    enable_smap_smep_umip();
    k_info("Supervisor memory protection enabled");

    gdt_install();
    k_info("GDT installed");
    init_interrupts();
    k_info("Interrupts enabled");

    struct limine_hhdm_response *response = hhdm_request.response;
    init_physical_allocator(response->offset, memmap_request);
    vmm_offset_set(response->offset);
    vmm_init();
    k_info("Virtual memory initialized");
    vmm_map_region(0x123123123, 0x7700, PAGING_WRITE);
    uint64_t *p = vmm_alloc_pages(6);
    k_info("Tested an allocation of 0x%x pages", 6);
    *p = 42;
    k_printf("P is %d, at address 0x%zx\n", *p, p);
    vmm_free_pages(p, 6);
    extern uint8_t read_cmos(uint8_t reg);
    k_info((read_cmos(0x0) & 1) == 1
               ? "Houston, Tranquility Base here. The Eagle has landed."
               : "If puns were deli meat, this would be the wurst.");
    debug_print_stack();
    struct task_t *t1 = create_task(task1);
    struct task_t *t2 = create_task(task2);
    struct task_t *t3 = create_task(task3);
    struct task_t *t4 = create_task(task4);
    struct task_t *t5 = create_task(task5);
    t1->next = t2;
    t2->next = t3;
    t3->next = t4;
    t4->next = t5;
    t5->next = t1;
    first_task = t1;
    current_task = first_task;
    asm volatile("mov %0, %%rsp\n"
                 "pop %%r15\n"
                 "pop %%r14\n"
                 "pop %%r13\n"
                 "pop %%r12\n"
                 "pop %%r11\n"
                 "pop %%r10\n"
                 "pop %%r9\n"
                 "pop %%r8\n"
                 "pop %%rbp\n"
                 "pop %%rdi\n"
                 "pop %%rsi\n"
                 "pop %%rdx\n"
                 "pop %%rcx\n"
                 "pop %%rbx\n"
                 "pop %%rax\n"
                 "sti\n"
                 "iretq\n"
                 :
                 : "r"(&current_task->regs)
                 : "memory");
    while (1) {
        asm("hlt");
    }
}
