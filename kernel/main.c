#include <acpi/acpi.h>
#include <acpi/cst.h>
#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <bootstage.h>
#include <charmos.h>
#include <compiler.h>
#include <console/printf.h>
#include <crypto/prng.h>
#include <elf.h>
#include <fs/vfs.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/domain.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/cmdline.h>
#include <misc/logo.h>
#include <registry.h>
#include <requests.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <smp/core.h>
#include <smp/domain.h>
#include <smp/smp.h>
#include <stdint.h>
#include <syscall.h>
#include <tests.h>

struct charmos_globals global = {0};

#define BEHAVIOR /* avoids undefined behavior */

void k_main(void) {
    global.core_count = mp_request.response->cpu_count;
    global.hhdm_offset = hhdm_request.response->offset;

    disable_interrupts();

    /* Framebuffer */
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    global.current_bootstage = BOOTSTAGE_EARLY_FB;

    k_printf("%s", OS_LOGO_SMALL);

    /* Early */
    smp_wakeup_processors(mp_request.response);
    smap_init();
    global.current_bootstage = BOOTSTAGE_EARLY_MP;

    /* Allocators */

    /* Get us up and running with a bitmap allocator */
    pmm_early_init(memmap_request);

    /* Paging, please! */
    vmm_init(memmap_request.response, xa_request.response);

    /* Switches us to a buddy allocator */
    pmm_mid_init();

    /* kmalloc can be used */
    slab_allocator_init();
    global.current_bootstage = BOOTSTAGE_EARLY_ALLOCATORS;

    gdt_install();
    syscall_setup(syscall_entry);
    smp_setup_bsp();

    /* Early devices */
    idt_init();
    uacpi_init(rsdp_request.response->address);
    x2apic_init();
    lapic_init();
    hpet_init();
    ioapic_init();
    acpi_find_cst();
    global.current_bootstage = BOOTSTAGE_EARLY_DEVICES;
    k_info("MAIN", K_INFO,
           "Early boot OK - %llu cores - total usable pages is 0x%llx",
           global.core_count, global.total_pages);

    /* Scheduler */
    thread_init_thread_ids();
    scheduler_init();
    reaper_init();
    workqueues_permanent_init();
    defer_init();
    prng_seed(time_get_us());
    global.current_bootstage = BOOTSTAGE_MID_SCHEDULER;
    k_info("MAIN", K_INFO, "Scheduler init OK");

    /* Command line + MP complete */
    cmdline_parse(cmdline_request.response->cmdline);
    lapic_timer_init(/* core_id = */ 0);
    smp_complete_init();
    srat_init();
    slit_init();
    topology_init();
    domain_init();

    /* NUMA awareness now */
    pmm_late_init();
    slab_domain_init();

    restore_interrupts();
    scheduler_yield();

    /* Should be unreachable */
    while (1) {
        wait_for_interrupt();
    }
}

void k_sch_main() {
    enable_interrupts();
    k_info("MAIN", K_INFO, "Device setup");
    global.current_bootstage = BOOTSTAGE_LATE_DEVICES;
    registry_setup();
    tests_run();
    k_info("MAIN", K_INFO, "Boot OK");
    global.current_bootstage = BOOTSTAGE_COMPLETE;

    thread_print(scheduler_get_current_thread());

    domain_buddy_dump();
    while (1) {
        wait_for_interrupt();
    }
}
