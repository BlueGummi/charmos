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
#include <elf.h>
#include <fs/vfs.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/cmdline.h>
#include <misc/logo.h>
#include <mp/core.h>
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

    // FB
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_hhdm_response *r = hhdm_request.response;
    k_printf("%s", OS_LOGO_SMALL);

    // Early init
    mp_wakeup_processors(mp_request.response);
    smap_init();
    global.current_bootstage = BOOTSTAGE_EARLY_MP;

    // Mem
    pmm_init(memmap_request);
    vmm_init(memmap_request.response, xa_request.response);
    slab_init();
    pmm_dyn_init();

    global.current_bootstage = BOOTSTAGE_EARLY_ALLOCATORS;
    gdt_install();

    syscall_setup(syscall_entry);
    mp_setup_bsp();

    // IDT
    idt_init();

    // Early device init
    uacpi_init(rsdp_request.response->address);
    lapic_init();

    hpet_init();
    ioapic_init();
    acpi_find_cst();

    global.current_bootstage = BOOTSTAGE_EARLY_DEVICES;
    k_info("MAIN", K_INFO, "Early boot OK - %llu cores", global.core_count);

    // Scheduler
    scheduler_init();
    defer_init();
    global.current_bootstage = BOOTSTAGE_MID_SCHEDULER;
    k_info("MAIN", K_INFO, "Scheduler init OK");

    // Filesystem init
    cmdline_parse(cmdline_request.response->cmdline);
    lapic_timer_init();
    mp_complete_init();

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
