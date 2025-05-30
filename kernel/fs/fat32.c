#include <alloc.h>
#include <disk.h>
#include <fs/fat32.h>
#include <fs/fat32_print.h>
#include <mbr.h>
#include <printf.h>
#include <string.h>

struct fat32_bpb *fat32_read_bpb(struct ide_drive *drive) {
    uint8_t *sector = kmalloc(SECTOR_SIZE);
    if (!sector)
        return NULL;

    if (!ide_read_sector(drive, 0, sector)) {
        kfree(sector);
        return NULL;
    }

    struct mbr *mbr = (struct mbr *) sector;
    uint32_t fat32_lba = 0;

    if (mbr->signature == 0xAA55) {
        for (int i = 0; i < 4; ++i) {
            uint8_t type = mbr->partitions[i].type;
            if (type == FAT32_PARTITION_TYPE1 ||
                type == FAT32_PARTITION_TYPE2) {
                fat32_lba = mbr->partitions[i].lba_start;
                break;
            }
        }
    }

    if (fat32_lba != 0 && ide_read_sector(drive, fat32_lba, sector)) {
        struct fat32_bpb *bpb = (struct fat32_bpb *) sector;

        if (bpb->boot_signature == 0x29 &&
            memcmp(bpb->fs_type, "FAT32   ", 8) == 0) {
            struct fat32_bpb *out_bpb = kmalloc(sizeof(struct fat32_bpb));
            if (out_bpb) {
                memcpy(out_bpb, bpb, sizeof(struct fat32_bpb));
                kfree(sector);
                return out_bpb;
            }
            kfree(sector);
            return NULL;
        }
    }

    for (uint32_t lba = 0; lba < 32; ++lba) {
        if (!ide_read_sector(drive, lba, sector))
            continue;

        struct fat32_bpb *bpb = (struct fat32_bpb *) sector;

        uint8_t jmp = sector[0];
        if (!((jmp == 0xEB && sector[2] == 0x90) || jmp == 0xE9))
            continue;

        if (bpb->boot_signature != 0x29)
            continue;

        if (memcmp(bpb->fs_type, "FAT32   ", 8) != 0)
            continue;

        struct fat32_bpb *out_bpb = kmalloc(sizeof(struct fat32_bpb));
        if (out_bpb) {
            memcpy(out_bpb, bpb, sizeof(struct fat32_bpb));
            kfree(sector);
            return out_bpb;
        }
        kfree(sector);
        return NULL;
    }

    kfree(sector);
    return NULL;
}
