#include <core.h>
#include <dbg.h>
#include <disk.h>
#include <ext2.h>
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
#include <vfs/vfs.h>
#include <vmalloc.h>
#include <vmm.h>

struct scheduler global_sched;
uint64_t t3_id = 0;

extern void test_alloc();

void k_sch_main() {
    k_printf("Welcome to the idle task!\n");
    while (1) {
        asm volatile("hlt");
    }
}

void k_main(void) {
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
    test_alloc();
    core_data = vmm_alloc_pages(1);
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3_ready = 1;
    while (current_cpu != mpr->cpu_count - 1) {
        asm volatile("pause");
    }
    struct ext2_sblock superblock;

    if (read_ext2_superblock(0, &superblock)) {
        k_printf("fire\n");
    } else {
        k_printf("ts pmo\n");
    }
    print_ext2_sblock(&superblock);


    struct thread *k_idle = thread_create(k_sch_main);
    global_sched.active = true;
    global_sched.started_first = false;
    scheduler_init(&global_sched);
    scheduler_add_thread(&global_sched, k_idle);
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
