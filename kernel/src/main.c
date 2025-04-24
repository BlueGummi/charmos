//#include "uacpi/internal/log.h"
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <limine.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/a_config.h>
#include <system/dbg.h>
#include <system/dsdt.h>
#include <system/fadt.h>
#include <system/gdt.h>
#include <system/idt.h>
#include <system/io.h>
#include <system/memfuncs.h>
#include <system/pmm.h>
#include <system/printf.h>
#include <system/rsdp.h>
#include <system/shutdown.h>
#include <system/smap.h>
#include <system/vmalloc.h>
#include <system/vmm.h>

__attribute__((used,
               section(".limine_requests_"
                       "start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((
    used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST, .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_executable_address_request addr_request = {
        .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST, .revision = 0};

__attribute__((
    used, section(".limine_requests"))) static volatile struct limine_mp_request
    mp_request = {.id = LIMINE_MP_REQUEST, .revision = 0};

__attribute__((
    used,
    section(
        ".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        asm("hlt");
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        asm("hlt");
    }

    struct limine_framebuffer *framebuffer =
        framebuffer_request.response->framebuffers[0];
    struct flanterm_context *ft_ctx = flanterm_fb_init(
        NULL, NULL, framebuffer->address, framebuffer->width,
        framebuffer->height, framebuffer->pitch, framebuffer->red_mask_size,
        framebuffer->red_mask_shift, framebuffer->green_mask_size,
        framebuffer->green_mask_shift, framebuffer->blue_mask_size,
        framebuffer->blue_mask_shift, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, 0, 0, 1, 0, 0, 0);
    asm volatile("cli");
    k_printf_init(ft_ctx);
    k_info("Framebuffer initialized");
    debug_print_stack();
    enable_smap_smep_umip();
    k_info("Supervisor memory protection enabled");

    gdt_install();
    k_info("GDT installed");
    init_interrupts();
    k_info("Interrupts enabled");

    struct limine_hhdm_response *response = hhdm_request.response;
    init_physical_allocator(response->offset, memmap_request);

    struct limine_mp_response mp_response;
    {
        struct limine_mp_response *mp = mp_request.response;
        mp_response.revision = mp->revision;
        mp_response.flags = mp->flags;
        mp_response.bsp_lapic_id = mp->bsp_lapic_id;
        mp_response.cpu_count = mp->cpu_count;
        mp_response.cpus = pmm_alloc_page(true);

        /*for (uint64_t i = 0; i < mp_response.cpu_count; i++) {
            mp_response.cpus[i]->processor_id = mp->cpus[i]->processor_id;
            mp_response.cpus[i]->lapic_id = mp->cpus[i]->lapic_id;
            mp_response.cpus[i]->reserved = mp->cpus[i]->reserved;
            mp_response.cpus[i]->goto_address = mp->cpus[i]->goto_address;
            mp_response.cpus[i]->extra_argument = mp->cpus[i]->extra_argument;
        }*/
    }

    vmm_offset_set(response->offset);

    vmm_init();
    vmm_map_region(0x123123123, 0x7700, PAGING_WRITE);
    vmm_map_region(0x4444446969, 0x33333, PAGING_XD);
    k_info("we have %d cores", mp_response.cpu_count);
    uint64_t *p = vmm_alloc_pages(6);
    k_info("Tested an allocation of 0x%x pages", 6);
    *p = 42;
    k_printf("P is %d, at address 0x%zx\n", *p, p);
    vmm_free_pages(p, 6);

    extern unsigned char keyboard_shift_map[128];
    extern unsigned char keyboard_map[128];
    k_info("Keyboard map at 0x%zx, shift map at 0x%zx", keyboard_map,
           keyboard_shift_map);

    extern uint8_t read_cmos(uint8_t reg);
    k_info((read_cmos(0x0) & 1) == 1
               ? "Houston, Tranquility Base here. The Eagle has landed."
               : "If puns were deli meat, this would be the wurst.");
    debug_print_stack();
    while (1) {
        asm("hlt");
    }
}
