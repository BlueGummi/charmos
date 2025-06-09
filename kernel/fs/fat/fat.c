#include <console/printf.h>
#include <devices/generic_disk.h>
#include <errno.h>
#include <fs/fat.h>
#include <fs/mbr.h>
#include <mem/alloc.h>
#include <string.h>

struct fat_bpb *fat_read_bpb(struct generic_disk *drive,
                             enum fat_fstype *out_type, uint32_t *out_lba) {
    uint8_t *sector = kmalloc(drive->sector_size);
    if (!sector)
        return NULL;

    uint32_t fat_lba = 0;

    if (drive->read_sector(drive, 0, sector, 1)) {
        struct mbr *mbr = (struct mbr *) sector;
        if (mbr->signature == 0xAA55) {
            for (int i = 0; i < 4; ++i) {
                uint8_t type = mbr->partitions[i].type;
                if (type == FAT32_PARTITION_TYPE1 ||
                    type == FAT32_PARTITION_TYPE2 ||
                    type == FAT16_PARTITION_TYPE ||
                    type == FAT12_PARTITION_TYPE) {
                    fat_lba = mbr->partitions[i].lba_start;
                    break;
                }
            }
        }
    }

    for (uint32_t lba = fat_lba; lba < fat_lba + 32; ++lba) {
        if (!drive->read_sector(drive, lba, sector, 1))
            continue;

        uint8_t jmp = sector[0];
        if (!((jmp == 0xEB && sector[2] == 0x90) || jmp == 0xE9))
            continue;

        struct fat_bpb *bpb = (struct fat_bpb *) sector;

        if (bpb->bytes_per_sector != 512 || bpb->num_fats == 0)
            continue;

        enum fat_fstype type = FAT_UNKNOWN;

        if (bpb->ext_32.boot_signature == 0x29 &&
            memcmp(bpb->ext_32.fs_type, "FAT32   ", 8) == 0) {
            type = FAT_32;
        } else if (bpb->ext_12_16.boot_signature == 0x29 &&
                   memcmp(bpb->ext_12_16.fs_type, "FAT16   ", 8) == 0) {
            type = FAT_16;
        } else if (bpb->ext_12_16.boot_signature == 0x29 &&
                   memcmp(bpb->ext_12_16.fs_type, "FAT12   ", 8) == 0) {
            type = FAT_12;
        }

        if (type == FAT_UNKNOWN)
            continue;

        struct fat_bpb *out_bpb = kmalloc(sizeof(struct fat_bpb));
        if (out_bpb) {
            memcpy(out_bpb, bpb, sizeof(struct fat_bpb));
            *out_type = type;
            *out_lba = lba;
            kfree(sector);
            return out_bpb;
        }

        break;
    }

    kfree(sector);
    return NULL;
}

enum errno fat_g_mount(struct generic_disk *d) {
    if (!d)
        return ERR_INVAL;

    struct fat_fs *fs = kmalloc(sizeof(struct fat_fs));
    if (!fs)
        return ERR_NO_MEM;

    enum fat_fstype type;
    uint32_t lba;
    struct fat_bpb *bpb = fat_read_bpb(d, &type, &lba);
    if (!bpb) {
        kfree(fs);
        return ERR_FS_INTERNAL;
    }

    fs->bpb = bpb;
    fs->type = type;
    fs->volume_base_lba = lba;
    fs->fat_size =
        (bpb->fat_size_16 != 0) ? bpb->fat_size_16 : bpb->ext_32.fat_size_32;
    fs->boot_signature = (type == FAT_32) ? bpb->ext_32.boot_signature
                                          : bpb->ext_12_16.boot_signature;
    fs->drive_number = (type == FAT_32) ? bpb->ext_32.drive_number
                                        : bpb->ext_12_16.drive_number;
    fs->volume_id =
        (type == FAT_32) ? bpb->ext_32.volume_id : bpb->ext_12_16.volume_id;

    memcpy(fs->fs_type,
           (type == FAT_32) ? bpb->ext_32.fs_type : bpb->ext_12_16.fs_type, 8);
    memcpy(fs->volume_label,
           (type == FAT_32) ? bpb->ext_32.volume_label
                            : bpb->ext_12_16.volume_label,
           11);

    d->fs_data = fs;
    return ERR_OK;
}

void fat_g_print(struct generic_disk *d) {
    if (!d || !d->fs_data)
        return;

    struct fat_fs *fs = d->fs_data;

    switch (fs->type) {
    case FAT_12:
    case FAT_16: fat16_print_bpb(fs->bpb); break;
    case FAT_32: fat32_print_bpb(fs->bpb); break;
    }

    fat_list_root(d);
}
