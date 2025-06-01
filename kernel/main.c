#include <acpi/print.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <devices/ahci.h>
#include <devices/generic_disk.h>
#include <devices/ide.h>
#include <devices/nvme.h>
#include <devices/registry.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <fs/fat32.h>
#include <fs/fat32_print.h>
#include <fs/supersector.h>
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
#include <requests.h>
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
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <vfs/vfs.h>

struct scheduler global_sched;

void k_sch_main() {
    k_printf("idle task\n");
    while (1) {
        asm volatile("hlt");
    }
}

uint64_t a_rsdp = 0;
uint64_t tsc_freq = 0;

void k_main(void) {
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_hhdm_response *r = hhdm_request.response;
    k_printf("%s", OS_LOGO_SMALL);
    a_rsdp = rsdp_request.response->address;
    mp_wakeup_processors(mp_request.response);
    enable_smap_smep_umip();
    gdt_install();
    idt_install();
    init_physical_allocator(r->offset, memmap_request);
    vmm_offset_set(r->offset);
    vmm_init();
    slab_init();
    test_alloc();
    tsc_freq = measure_tsc_freq_pit();
    uacpi_initialize(0);
    uacpi_namespace_load();
    uacpi_namespace_initialize();

    /*    uacpi_namespace_for_each_child(uacpi_namespace_root(), acpi_print_ctx,
                                       UACPI_NULL, UACPI_OBJECT_DEVICE_BIT,
                                       UACPI_MAX_DEPTH_ANY, UACPI_NULL);*/

    registry_setup();
    registry_print_devices();
    registry_detect_fs();

    scheduler_init(&global_sched);
    scheduler_add_thread(&global_sched, thread_create(k_sch_main));
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
