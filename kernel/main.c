#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
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
#include <requests.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall.h>

struct scheduler global_sched = {0};
uint64_t a_rsdp = 0;
char *g_root_part = "";
struct vfs_node *g_root_node = NULL;
struct vfs_mount *g_mount_list_head; // TODO: migrate these globals

#define BEHAVIOR /* avoids undefined behavior */

void k_main(void) {
    uint64_t c_cnt = mp_request.response->cpu_count;

    // FB
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_hhdm_response *r = hhdm_request.response;
    k_printf("%s", OS_LOGO_SMALL);
    a_rsdp = rsdp_request.response->address;

    // Early init
    mp_wakeup_processors(mp_request.response);
    smap_init();

    // Mem
    pmm_init(r->offset, memmap_request);
    vmm_init(memmap_request.response, xa_request.response, r->offset);
    slab_init();
    pmm_dyn_init();
    gdt_install();

    syscall_setup(syscall_entry);
    mp_setup_bsp(c_cnt);

    // IDT
    idt_alloc(c_cnt);
    idt_install(0);

    // Early device init
    uacpi_init();
    lapic_init();

    hpet_init();
    ioapic_init();

    k_info("MAIN", K_INFO, "Early boot OK");

    // Scheduler
    scheduler_init(c_cnt);
    defer_init();
    k_info("MAIN", K_INFO, "Scheduler init OK");

    // Filesystem init
    cmdline_parse(cmdline_request.response->cmdline);
    lapic_timer_init();
    mp_complete_init();

    restore_interrupts();
    scheduler_yield();

    while (1) {
        asm("hlt");
    }
}
