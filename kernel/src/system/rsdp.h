#include <stdint.h>

struct __attribute__((packed)) Rsdp {
    uint8_t signature[8];
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address; // ACPI 1.0
};

extern struct Rsdp make_rsdp(void *rsdp_addr);
