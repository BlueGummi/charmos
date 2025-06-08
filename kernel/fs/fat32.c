#include <console/printf.h>
#include <devices/generic_disk.h>
#include <errno.h>
#include <fs/fat32.h>
#include <fs/fat32_print.h>
#include <fs/mbr.h>
#include <mem/alloc.h>
#include <string.h>

struct fat_bpb *fat32_read_bpb(struct generic_disk *drive) {
    uint8_t *sector = kmalloc(SECTOR_SIZE);

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
    return ERR_OK; // TODO: Mounting
}

uint32_t fat32_first_data_sector(const struct fat_bpb *bpb) {
    return bpb->reserved_sector_count +
           (bpb->num_fats * bpb->ext_32.fat_size_32);
}

uint32_t fat32_cluster_to_lba(const struct fat_bpb *bpb, uint32_t cluster) {
    return fat32_first_data_sector(bpb) +
           (cluster - 2) * bpb->sectors_per_cluster;
}

bool fat32_read_cluster(struct generic_disk *disk, uint32_t cluster,
                        uint8_t *buffer) {
    struct fat_fs *fs = disk->fs_data;
    struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat32_cluster_to_lba(bpb, cluster);
    return disk->read_sector(disk, lba, buffer, bpb->sectors_per_cluster);
}

void fat32_list_root(struct generic_disk *disk) {
    struct fat_fs *fs = disk->fs_data;
    if (!fs || !fs->bpb)
        return;

    uint32_t cluster_size =
        fs->bpb->sectors_per_cluster * fs->bpb->bytes_per_sector;
    uint8_t *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf)
        return;

    uint32_t cluster = fs->bpb->ext_32.root_cluster;

    if (fat32_read_cluster(disk, cluster, cluster_buf)) {
        for (uint32_t i = 0; i < cluster_size; i += sizeof(struct fat_dirent)) {
            struct fat_dirent *entry = (struct fat_dirent *) (cluster_buf + i);
            fat32_print_dirent(entry);
        }
    }

    kfree(cluster_buf);
}

void fat32_g_print(struct generic_disk *d) {
    if (!d)
        return;
    struct fat_fs *fs = (struct fat_fs *) d->fs_data;
    fat32_print_bpb(fs->bpb);
    fat32_list_root(d);
}
