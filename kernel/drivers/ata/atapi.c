#include <asm.h>
#include <drivers/ata.h>
#include <mem/alloc.h>

#define ATAPI_SECTOR_SIZE 2048

bool atapi_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                       uint64_t sector_count) {
    if (sector_count != 1)
        return false;

    struct ide_drive *atapi = (struct ide_drive *) disk->driver_data;
    uint16_t io = atapi->io_base;

    outb(REG_DRIVE_HEAD(io), atapi->slave ? 0xB0 : 0xA0);
    io_wait();

    outb(REG_FEATURES(io), 0);
    outb(REG_LBA_MID(io), ATAPI_SECTOR_SIZE & 0xFF);
    outb(REG_LBA_HIGH(io), ATAPI_SECTOR_SIZE >> 8);

    outb(REG_COMMAND(io), ATA_CMD_PACKET);

    while (inb(REG_STATUS(io)) & STATUS_BSY)
        ;
    if (!(inb(REG_STATUS(io)) & STATUS_DRQ))
        return false;

    uint8_t packet[12] = {0x28, // READ (10)
                          0,
                          (lba >> 24) & 0xFF,
                          (lba >> 16) & 0xFF,
                          (lba >> 8) & 0xFF,
                          (lba >> 0) & 0xFF,
                          0,
                          0,
                          1, // 1 sector
                          0,
                          0,
                          0};

    for (int i = 0; i < 6; i++) {
        uint16_t word = ((uint16_t *) packet)[i];
        outw(REG_DATA(io), word);
    }

    while (true) {
        uint8_t status = inb(REG_STATUS(io));
        if (status & STATUS_ERR)
            return false;
        if (status & STATUS_DRQ)
            break;
    }

    for (int i = 0; i < ATAPI_SECTOR_SIZE / 2; i++) {
        uint16_t word = inw(REG_DATA(io));
        buffer[i * 2] = word & 0xFF;
        buffer[i * 2 + 1] = (word >> 8) & 0xFF;
    }

    for (int i = 0; i < 4; i++)
        inb(REG_STATUS(io));

    return true;
}

bool atapi_write_sector(struct generic_disk *disk, uint64_t lba,
                        const uint8_t *buffer, uint64_t sector_count) {
    (void) disk;
    (void) lba;
    (void) buffer;
    (void) sector_count;
    return false;
}

bool atapi_read_sector_wrapper(struct generic_disk *disk, uint64_t start_lba,
                               uint8_t *buffer, uint64_t sector_count) {
    uint8_t *buf_ptr = buffer;
    for (uint64_t i = 0; i < sector_count; i++) {
        if (!atapi_read_sector(disk, start_lba + i, buf_ptr, 1)) {
            return false;
        }
        buf_ptr += ATAPI_SECTOR_SIZE;
    }
    return true;
}

void atapi_print_info(struct generic_disk *disk) {
    struct ide_drive *d = disk->driver_data;
    ata_ident_print(d->identify_data);
}

struct generic_disk *atapi_create_generic(struct ide_drive *d) {
    struct generic_disk *ret = kmalloc(sizeof(struct generic_disk));
    ret->driver_data = d;
    ret->sector_size = 2048;
    ret->read_sector = atapi_read_sector_wrapper;
    ret->write_sector = atapi_write_sector;
    ret->print = atapi_print_info;
    ret->type = G_ATAPI_DRIVE;
    return ret;
}
