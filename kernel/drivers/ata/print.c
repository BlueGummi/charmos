#include <console/printf.h>
#include <drivers/ata.h>

static void print_string_field(uint16_t *field, int length) {
    char buf[length * 2 + 1];
    for (int i = 0; i < length; ++i) {
        buf[i * 2] = (field[i] >> 8) & 0xFF;
        buf[i * 2 + 1] = field[i] & 0xFF;
    }
    buf[length * 2] = '\0';

    for (int i = length * 2 - 1; i >= 0 && buf[i] == ' '; i--) {
        buf[i] = '\0';
    }

    k_printf("%s", buf);
}

void ata_ident_print(struct ata_identify *id) {
    k_printf("== ATA IDENTIFY INFO ==\n");

    k_printf("Model Number: ");
    print_string_field(id->model_number, 20);
    k_printf("\n");

    k_printf("Serial Number: ");
    print_string_field(id->serial_number, 10);
    k_printf("\n");

    k_printf("Firmware Revision: ");
    print_string_field(id->firmware_revision, 4);
    k_printf("\n");

    k_printf("LBA28 Sector Count: %u\n", id->lba28_capacity);
    k_printf("LBA48 Sector Count: %llu\n",
             (unsigned long long) id->lba48_sector_count);

    k_printf("Capabilities: 0x%04X 0x%04X\n", id->capabilities[0],
             id->capabilities[1]);
    k_printf("DMA Supported: 0x%04X\n", id->dma_supported);
    k_printf("PIO Modes: 0x%04X\n", id->advanced_pio_modes);

    k_printf("SATA Capabilities: 0x%04X\n", id->sata_capabilities);
    k_printf("SATA Features Supported: 0x%04X\n", id->sata_features_supported);
    k_printf("SATA Features Enabled: 0x%04X\n", id->sata_features_enabled);

    k_printf("Major Version: 0x%04X\n", id->major_version);
    k_printf("Minor Version: 0x%04X\n", id->minor_version);

    k_printf("========================\n");
}
