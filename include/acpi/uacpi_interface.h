#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <uacpi/internal/types.h>

typedef struct { // typedefing it because of consistency with uacpi naming
    uint8_t bus, slot, func;
    bool is_open;
} uacpi_pci_device;

typedef struct {
    uacpi_io_addr base;
    uacpi_size len;
    bool valid;
} uacpi_io_handle;

typedef struct {
    uacpi_interrupt_handler handler;
    uacpi_handle ctx;
    bool installed;
} irq_entry_t;

void uacpi_mark_irq_installed(uint8_t irq);
void uacpi_init();
void uacpi_print_devs();
