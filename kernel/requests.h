#include <compiler.h>
#include <limine.h>
__attribute__((used,
               section(".limine_requests_"
                       "start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((
    used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);

//
//
// START
//
//

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 3};

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST, .revision = 3};

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_executable_address_request
    xa_request = {.id = LIMINE_EXECUTABLE_ADDRESS_REQUEST, .revision = 3};

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_hhdm_request hhdm_request =
    {.id = LIMINE_HHDM_REQUEST, .revision = 3};

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_rsdp_request rsdp_request =
    {.id = LIMINE_RSDP_REQUEST, .revision = 3};

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST, .revision = 3};

__attribute__((used, section(".limine_requests")))
__no_sanitize_address static volatile struct limine_executable_cmdline_request
    cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 3};

//
//
// END
//
//
__attribute__((
    used,
    section(
        ".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;
