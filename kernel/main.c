#include <acpi/acpi.h>
#include <acpi/cst.h>
#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <bootstage.h>
#include <cmdline.h>
#include <compiler.h>
#include <console/printf.h>
#include <crypto/prng.h>
#include <drivers/iommu/iommu.h>
#include <drivers/mmio.h>
#include <elf.h>
#include <fs/vfs.h>
#include <global.h>
#include <irq/idt.h>
#include <limine.h>
#include <log.h>
#include <logo.h>
#include <mem/address_range.h>
#include <mem/alloc.h>
#include <mem/asan.h>
#include <mem/buddy.h>
#include <mem/domain.h>
#include <mem/movealloc.h>
#include <mem/page_alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <ndjson.h>
#include <registry.h>
#include <requests.h>
#include <sch/domain.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/perdomain.h>
#include <smp/pernode.h>
#include <smp/smp.h>
#include <stack_depot.h>
#include <stdint.h>
#include <sync/rcu.h>
#include <sync/turnstile.h>
#include <syscall.h>
#include <test/test.h>
#include <thread/dpc.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/workqueue.h>
#include <time/clock.h>
#include <time/timekeeper.h>
#include <time/timer.h>
#include <watchdog.h>

struct globals global = {0};

#define BEHAVIOR /* avoids undefined behavior */

__no_sanitize_address void k_main(void) {
    disable_interrupts();
    global.core_count = mp_request.response->cpu_count;
    global.hhdm_offset = hhdm_request.response->offset;
    global.pt_epoch = 1;

    err_facilities_init();
    printf_init(framebuffer_request.response->framebuffers[0]);
    ndjson_early_init();
    bootstage_advance(BOOTSTAGE_EARLY_FB);

    const char *cmdline =
        cmdline_request.response ? cmdline_request.response->cmdline : "";

    if (cmdline_wants_help(cmdline))
        cmdline_dump_help();

    pmm_early_init(memmap_request);
    vmm_init(memmap_request.response, xa_request.response);
    pmm_mid_init();

#ifdef DEBUG_ASAN
    asan_init();
#endif

    address_ranges_init();
    slab_allocator_init();
    page_alloc_init();

    stack_depot_init();
    log_sites_init();
    bootstage_advance(BOOTSTAGE_EARLY_ALLOCATORS);
    gdt_install();
    syscall_setup(syscall_entry);
    smp_setup_bsp();

    clocks_init();

    mmio_init();
    irq_init();
    uacpi_init(rsdp_request.response->address);
    x2apic_init();
    lapic_init();
    hpet_init();
    ioapic_init();
    acpi_find_cst();
    bootstage_advance(BOOTSTAGE_EARLY_DEVICES);

    srat_init();
    slit_init();
    iommu_init();

    domain_init();
    pmm_late_init();
    slab_domain_init();

    percpu_obj_init();
    watchdog_init();
    smp_init();

    domain_init_after_smp();
    domain_buddies_init_after_smp();
    thread_init_thread_ids();

    scheduler_init();
    turnstiles_init();

    cmdline_parse(cmdline);
    ndjson_init();
    cmdline_debug_hook();

    lapic_timer_init_bsp();
    dpc_init_percpu();
    smp_wake(mp_request.response);
    timekeeper_init();

    topology_init();
    scheduler_domains_init();
    bootstage_advance(BOOTSTAGE_MID_TOPOLOGY);

    perdomain_obj_init();
    pernode_obj_init();

    lapic_clock_evdev_group_init();
    timers_init();
    watchdog_start();

    scheduler_periodic_work_init();
    movealloc_exec_all();
    bootstage_advance(BOOTSTAGE_MID_ALLOCATORS);

    scheduler_yield();
}

void k_sch_main(void *nop) {
    (void) nop;
    /* make sure everyone else is idle before we
     * advance the bootstage here... */
    smp_wait_for_others_to_idle();

    /* we have to force everyone to disable their
     * ticks because this prevents anyone from
     * possibly entering an ISR since IRQL
     * operations check bootstages and they can
     * see the MID_ALLOCATORS bootstage (no-op) in
     * an early `irql_raise` and the LATE_DEVICES
     * bootstage later on (causing mis-raised IRQLs) */
    smp_disable_all_ticks();

    bootstage_advance(BOOTSTAGE_LATE);

    smp_enable_all_ticks();

    rcu_init();
    workqueues_permanent_init();
    slab_domain_init_late();
    domain_buddies_init_late();
    reaper_init();

    registry_setup();
    tests_run();
    bootstage_advance(BOOTSTAGE_COMPLETE);

#ifdef TEST_ENABLED
    return;
#endif

    thread_print(thread_get_current());

    domain_buddy_dump();
}
