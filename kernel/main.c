#include <acpi/acpi.h>
#include <acpi/cst.h>
#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <boot/stage.h>
#include <charmos.h>
#include <compiler.h>
#include <console/printf.h>
#include <crypto/prng.h>
#include <elf.h>
#include <fs/vfs.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/cmdline.h>
#include <misc/logo.h>
#include <mp/core.h>
#include <mp/domain.h>
#include <mp/mp.h>
#include <registry.h>
#include <requests.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdint.h>
#include <syscall.h>
#include <tests.h>

struct charmos_globals global = {0};
struct spinlock panic_lock = SPINLOCK_INIT;

#define BEHAVIOR /* avoids undefined behavior */

void k_main(void) {
    global.core_count = mp_request.response->cpu_count;
    global.hhdm_offset = hhdm_request.response->offset;

    /* Framebuffer */
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    k_printf("%s", OS_LOGO_SMALL);

    /* Early */
    mp_wakeup_processors(mp_request.response);
    smap_init();
    global.current_bootstage = BOOTSTAGE_EARLY_MP;

    /* Allocators */
    pmm_early_init(memmap_request);

    vmm_init(memmap_request.response, xa_request.response);
    pmm_mid_init();

    slab_init();
    hugepage_alloc_init();
    global.current_bootstage = BOOTSTAGE_EARLY_ALLOCATORS;

    gdt_install();
    syscall_setup(syscall_entry);
    mp_setup_bsp();

    /* Early devices */
    idt_init();
    uacpi_init(rsdp_request.response->address);
    x2apic_init();
    lapic_init();
    hpet_init();
    ioapic_init();
    acpi_find_cst();
    global.current_bootstage = BOOTSTAGE_EARLY_DEVICES;
    k_info("MAIN", K_INFO, "Early boot OK - %llu cores", global.core_count);

    /* Scheduler */
    scheduler_init();
    defer_init();
    prng_seed(time_get_us());
    global.current_bootstage = BOOTSTAGE_MID_SCHEDULER;
    k_info("MAIN", K_INFO, "Scheduler init OK");

    /* Command line + MP complete */
    cmdline_parse(cmdline_request.response->cmdline);
    lapic_timer_init(0);

    mp_complete_init();
    srat_init();
    slit_init();
    topology_init();
    core_domain_init();

    restore_interrupts();
    scheduler_yield();

    while (1) {
        wait_for_interrupt();
    }
}

void k_sch_main() {
    k_info("MAIN", K_INFO, "Device setup");
    registry_setup();
    global.current_bootstage = BOOTSTAGE_LATE_DEVICES;
    tests_run();
    k_info("MAIN", K_INFO, "Boot OK");
    global.current_bootstage = BOOTSTAGE_COMPLETE;

    thread_log_event_reasons(scheduler_get_curr_thread());
    while (1) {
        wait_for_interrupt();
    }
}
