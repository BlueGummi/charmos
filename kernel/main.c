#include <acpi/acpi.h>
#include <acpi/cst.h>
#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <bootstage.h>
#include <charmos.h>
#include <cmdline.h>
#include <compiler.h>
#include <console/printf.h>
#include <crypto/prng.h>
#include <elf.h>
#include <fs/vfs.h>
#include <int/idt.h>
#include <limine.h>
#include <logo.h>
#include <mem/alloc.h>
#include <mem/asan.h>
#include <mem/buddy.h>
#include <mem/domain.h>
#include <mem/movealloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <registry.h>
#include <requests.h>
#include <sch/defer.h>
#include <sch/domain.h>
#include <sch/dpc.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <smp/core.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/smp.h>
#include <stdint.h>
#include <sync/rcu.h>
#include <sync/turnstile.h>
#include <syscall.h>
#include <tests.h>

struct charmos_globals global = {0};

#define BEHAVIOR /* avoids undefined behavior */

__no_sanitize_address void k_main(void) {
    global.core_count = mp_request.response->cpu_count;
    global.hhdm_offset = hhdm_request.response->offset;

    disable_interrupts();

    k_printf_init(framebuffer_request.response->framebuffers[0]);
    bootstage_advance(BOOTSTAGE_EARLY_FB);

    smp_wakeup_processors(mp_request.response);
    bootstage_advance(BOOTSTAGE_EARLY_MP);

    pmm_early_init(memmap_request);
    vmm_init(memmap_request.response, xa_request.response);
    pmm_mid_init();

    slab_allocator_init();
    asan_init();
    bootstage_advance(BOOTSTAGE_EARLY_ALLOCATORS);
    gdt_install();
    syscall_setup(syscall_entry);
    smp_setup_bsp();

    idt_init();
    uacpi_init(rsdp_request.response->address);
    x2apic_init();
    lapic_init();
    hpet_init();
    ioapic_init();
    acpi_find_cst();
    bootstage_advance(BOOTSTAGE_EARLY_DEVICES);

    thread_init_thread_ids();
    scheduler_init();
    turnstiles_init();
    prng_seed(time_get_us());
    bootstage_advance(BOOTSTAGE_MID_SCHEDULER);

    cmdline_parse(cmdline_request.response->cmdline);
    lapic_timer_init(/* core_id = */ 0);
    dpc_init_percpu();
    smp_complete_init();
    percpu_obj_init();

    srat_init();
    slit_init();
    topology_init();
    domain_init();
    scheduler_domains_init();
    bootstage_advance(BOOTSTAGE_MID_TOPOLOGY);

    thread_init_rq_lists();

    pmm_late_init();
    slab_domain_init();
    movealloc_exec_all();
    bootstage_advance(BOOTSTAGE_MID_ALLOCATORS);

    scheduler_yield();
}

void k_sch_main(void *nop) {
    (void) nop;
    /* make sure everyone else is idle before we
     * advance the bootstage here... */
    smp_wait_for_others_to_idle();
    bootstage_advance(BOOTSTAGE_LATE_DEVICES);

    rcu_init();
    defer_init();
    slab_domain_init_late();
    domain_buddies_init_late();
    workqueues_permanent_init();
    reaper_init();
    registry_setup();
    tests_run();
    bootstage_advance(BOOTSTAGE_COMPLETE);

    thread_print(scheduler_get_current_thread());

    domain_buddy_dump();
}
