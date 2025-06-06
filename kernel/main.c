#include "requests.h"
#include <acpi/lapic.h>
#include <acpi/print.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <drivers/ahci.h>
#include <devices/generic_disk.h>
#include <drivers/ide.h>
#include <drivers/nvme.h>
#include <devices/registry.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <fs/fat32.h>
#include <fs/fat32_print.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/dbg.h>
#include <misc/linker_symbols.h>
#include <misc/logo.h>
#include <mp/core.h>
#include <mp/mp.h>
#include <pci/pci.h>
#include <pit.h>
#include <rust.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <spin_lock.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time/print.h>
#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <vfs/vfs.h>

struct scheduler global_sched;
uint64_t a_rsdp = 0;

void k_main(void) {
    uint64_t c_cnt = mp_request.response->cpu_count;
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_hhdm_response *r = hhdm_request.response;
    k_printf("%s", OS_LOGO_SMALL);
    a_rsdp = rsdp_request.response->address;
    mp_wakeup_processors(mp_request.response);
    enable_smap_smep_umip();
    gdt_install();
    init_physical_allocator(r->offset, memmap_request);
    vmm_offset_set(r->offset);
    vmm_init();
    slab_init();
    idt_alloc(c_cnt);
    idt_install(0);
    test_alloc();
    uacpi_init();
    registry_setup();

    struct generic_disk *d = registry_get_by_index(
        1); // if i did it right there should be a ext2 here
    struct ext2_sblock superblock;
    ext2_read_superblock(d, 0, &superblock);
    ext2_test(d, &superblock);

    scheduler_init(&global_sched, c_cnt);

    for (uint64_t i = 0; i < c_cnt; i++) {
        struct per_core_scheduler *s =
            kmalloc(sizeof(struct per_core_scheduler));
        scheduler_local_init(s, i);
        struct thread *t = thread_create(k_sch_main);
        scheduler_add_thread(&global_sched, t);
    }

    //    struct thread *t = thread_create(registry_print_devices);
    //    scheduler_add_thread(&global_sched, t);

    while (1) {
        asm("hlt");
    } // no do sched for now :boom:

    struct core *c = kmalloc(sizeof(struct core));
    c->state = IDLE;
    c->id = 0;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    scheduler_rebalance(&global_sched);
    lapic = vmm_map_phys(0xFEE00000UL, 4096);
    lapic_init();
    mp_inform_of_cr3();

    asm volatile("sti");
    scheduler_start(local_schs[0]);
    while (1) {
        asm("hlt");
    }
}
