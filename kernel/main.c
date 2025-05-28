#include <core.h>
#include <dbg.h>
#include <disk.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <limine.h>
#include <misc/logo.h>
#include <mp.h>
#include <pit.h>
#include <pci.h>
#include <pmm.h>
#include <printf.h>
#include <requests.h>
#include <rust.h>
#include <sched.h>
#include <shutdown.h>
#include <smap.h>
#include <spin_lock.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <thread.h>
#include <uacpi/event.h>
#include <uacpi/uacpi.h>
#include <vfs/vfs.h>
#include <vmalloc.h>
#include <vmm.h>

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
    struct limine_mp_response *mpr = mp_request.response;

    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }

    enable_smap_smep_umip();
    gdt_install();
    idt_install();
    init_physical_allocator(r->offset, memmap_request);
    vmm_offset_set(r->offset);
    vmm_init();

    tsc_freq = measure_tsc_freq_pit();
    uacpi_status ret = uacpi_initialize(0);
    if (uacpi_unlikely_error(ret)) {
        k_printf("uacpi_initialize error: %s\n", uacpi_status_to_string(ret));
    }
    ret = uacpi_namespace_load();
    if (uacpi_unlikely_error(ret)) {
        k_printf("uacpi_namespace_load error: %s", uacpi_status_to_string(ret));
    }
    ret = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(ret)) {
        k_printf("uacpi_namespace_initialize error: %s",
                 uacpi_status_to_string(ret));
    }
    ret = uacpi_finalize_gpe_initialization();
    if (uacpi_unlikely_error(ret)) {
        k_printf("uACPI GPE initialization error: %s",
                 uacpi_status_to_string(ret));
    }

    test_alloc();
    core_data = vmm_alloc_pages(1);
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3_ready = 1;
    while (current_cpu != mpr->cpu_count - 1) {
        asm volatile("pause");
    }

    struct pci_device *devices;
    uint64_t count;
    scan_pci_devices(&devices, &count);
    struct ide_drive primary_master;
    setup_primary_ide(&primary_master, devices, count);
    struct ext2_sblock superblock;

    if (read_ext2_superblock(&primary_master, 0, &superblock)) {
        ext2_test(&primary_master, &superblock);
    }

    //   uacpi_namespace_load();
    //   uacpi_namespace_initialize();

    scheduler_init(&global_sched);
    scheduler_add_thread(&global_sched, thread_create(k_sch_main));
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
