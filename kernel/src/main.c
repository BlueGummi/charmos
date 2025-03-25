#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <limine.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/a_config.h>
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

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests"))) static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_executable_address_request addr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0};

__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

void kmain(void) {
    struct limine_hhdm_response *response = hhdm_request.response;
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        asm("hlt");
    }

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        asm("hlt");
    }

    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    struct flanterm_context *ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        framebuffer->address, framebuffer->width, framebuffer->height, framebuffer->pitch,
        framebuffer->red_mask_size, framebuffer->red_mask_shift,
        framebuffer->green_mask_size, framebuffer->green_mask_shift,
        framebuffer->blue_mask_size, framebuffer->blue_mask_shift,
        NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, 0, 0, 1,
        0, 0,
        0);
    k_printf_init(ft_ctx);
    enable_smap_smep_umip();
    gdt_install();
    asm volatile("cli");
    init_physical_allocator(response->offset, memmap_request);
    vmm_offset_set(response->offset);
    vmm_init();
    k_info("VMM initialized");
    uint64_t *p = vmm_alloc_pages(69);
    k_info("alloc worked :D, i sit at 0x%zx", p);
    *p = 42;
    k_printf("P is %d\n", *p);
    vmm_free_pages(p, 1);
    k_info("Using fallback VM shutdown method");
    init_interrupts();

    while (1) {
        asm("hlt");
    }
}
