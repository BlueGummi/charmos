#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/ide.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <stdbool.h>
#include <stdint.h>

bool ide_setup_drive(struct ide_drive *ide, struct pci_device *devices,
                     uint64_t count, int channel, int is_slave) {
    ide->sector_size = 512;

    for (uint64_t i = 0; i < count; i++) {
        struct pci_device *curr = &devices[i];

        if (curr->class_code == 1 && curr->subclass == 1) {
            uint32_t bar = pci_read_bar(curr->bus, curr->device, curr->function,
                                        channel * 2);
            uint32_t ctrl_bar = pci_read_bar(curr->bus, curr->device,
                                             curr->function, channel * 2 + 1);

            if ((bar & 1) == 1) {
                ide->io_base = (uint16_t) (bar & 0xFFFFFFFC);
            } else {
                ide->io_base =
                    (channel == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
            }

            if ((ctrl_bar & 1) == 1) {
                ide->ctrl_base = (uint16_t) (ctrl_bar & 0xFFFFFFFC);
            } else {
                ide->ctrl_base =
                    (channel == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_IO;
            }

            ide->slave = is_slave;
            return true;
        }
    }

    ide->io_base = 0;
    ide->ctrl_base = 0;
    ide->slave = 0;
    return false;
}

struct generic_disk *ide_create_generic(struct ide_drive *ide) {
    ide_identify(ide);
    if (!ide->actually_exists)
        return NULL;

    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    d->type = G_IDE_DRIVE;
    d->driver_data = ide;
    d->sector_size = ide->sector_size;
    d->read_sector = ide_read_sector_wrapper;
    d->write_sector = ide_write_sector_wrapper;
    d->print = ide_print_info;
    return d;
}
