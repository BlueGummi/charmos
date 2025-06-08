#include <console/printf.h>
#include <devices/generic_disk.h>
#include <errno.h>
#include <fs/fat.h>
#include <fs/fat32_print.h>
#include <fs/mbr.h>
#include <mem/alloc.h>
#include <string.h>

struct fat_bpb *fat32_read_bpb(struct generic_disk *drive) {
    uint8_t *sector = kmalloc(drive->sector_size);

    if (!drive->read_sector(drive, 0, sector, 1)) {
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

    if (fat32_lba != 0 && drive->read_sector(drive, fat32_lba, sector, 1)) {
        struct fat_bpb *bpb = (struct fat_bpb *) sector;

        if (bpb->ext_32.boot_signature == 0x29 &&
            memcmp(bpb->ext_32.fs_type, "FAT32   ", 8) == 0) {
            struct fat_bpb *out_bpb = kmalloc(sizeof(struct fat_bpb));
            if (out_bpb) {
                memcpy(out_bpb, bpb, sizeof(struct fat_bpb));
                kfree(sector);
                return out_bpb;
            }
            kfree(sector);
            return NULL;
        }
    }

    for (uint32_t lba = 0; lba < 32; ++lba) {
        if (!drive->read_sector(drive, lba, sector, 1))
            continue;

        struct fat_bpb *bpb = (struct fat_bpb *) sector;

        uint8_t jmp = sector[0];
        if (!((jmp == 0xEB && sector[2] == 0x90) || jmp == 0xE9))
            continue;

        if (bpb->ext_32.boot_signature != 0x29)
            continue;

        if (memcmp(bpb->ext_32.fs_type, "FAT32   ", 8) != 0)
            continue;

        struct fat_bpb *out_bpb = kmalloc(sizeof(struct fat_bpb));
        if (out_bpb) {
            memcpy(out_bpb, bpb, sizeof(struct fat_bpb));
            kfree(sector);
            return out_bpb;
        }
        kfree(sector);
        return NULL;
    }

    kfree(sector);
    return NULL;
}

enum errno fat32_g_mount(struct generic_disk *d) {
    if (!d)
        return ERR_INVAL;
    d->fs_data = kmalloc(sizeof(struct fat_fs));
    struct fat_fs *fs = (struct fat_fs *) d->fs_data;
    fs->bpb = kmalloc(sizeof(struct fat_bpb));
    fs->bpb = fat32_read_bpb(d);
    fs->boot_signature = fs->bpb->ext_32.boot_signature;
    fs->drive_number = fs->bpb->ext_32.drive_number;
    fs->fat_size = fs->bpb->ext_32.fat_size_32;
    for (int i = 0; i < 8; i++)
        fs->fs_type[i] = fs->bpb->ext_32.fs_type[i];

    fs->type = FAT_32;
    fs->volume_id = fs->bpb->ext_32.volume_id;

    for (int i = 0; i < 11; i++)
        fs->volume_label[i] = fs->bpb->ext_32.volume_label[i];

    return ERR_OK; // TODO: Mounting
}

void fat32_g_print(struct generic_disk *d) {
    if (!d)
        return;
    struct fat_fs *fs = (struct fat_fs *) d->fs_data;
    fat32_print_bpb(fs->bpb);
    fat32_list_root(d);
}
