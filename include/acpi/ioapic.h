#pragma once
#include <stdint.h>
struct ioapic_info {
    uint8_t id;
    uint32_t gsi_base;
    uint32_t *mmio_base;
};

void ioapic_init(void);
