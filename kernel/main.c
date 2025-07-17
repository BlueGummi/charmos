#include <compiler.h>
#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <elf.h>
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
#include <sch/defer.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall.h>

#include "fs/vfs.h"
#include "requests.h"

struct scheduler global_sched = {0};
uint64_t a_rsdp = 0;
char *g_root_part = "";
struct vfs_node *g_root_node = NULL;
struct vfs_mount *g_mount_list_head; // TODO: migrate these globals

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
    struct core *c = kmalloc(sizeof(struct core));
    if (!c)
        k_panic("Could not allocate space for core structure on BSP");

    c->state = IDLE;
    c->current_thread = kzalloc(sizeof(struct thread));
    if (unlikely(!c->current_thread))
        k_panic("Could not allocate space for BSP's current thread");

    c->id = 0;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    global_cores = kmalloc(sizeof(struct core *) * c_cnt);
    if (unlikely(!global_cores))
        k_panic("Could not allocate space for global core structures");

    // IDT
    idt_alloc(c_cnt);
    idt_install(0);

    // Early device init
    uacpi_init();
    uintptr_t lapic_phys = rdmsr(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MASK;
    lapic = vmm_map_phys(lapic_phys, PAGE_SIZE, PAGING_UNCACHABLE);

    hpet_init();
    ioapic_init();
    k_info("MAIN", K_INFO, "Early boot OK");

    // Scheduler
    scheduler_init(c_cnt);
    defer_init();
    k_info("MAIN", K_INFO, "Scheduler init OK");

    // Filesystem init
    cmdline_parse(cmdline_request.response->cmdline);
    lapic_init();
    mp_inform_of_cr3();

    restore_interrupts();
    scheduler_yield();
    while (1) {
        asm("hlt");
    }
}
