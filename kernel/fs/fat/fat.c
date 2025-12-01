#include <block/generic.h>
#include <console/printf.h>
#include <fs/fat.h>
#include <fs/mbr.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void fat_write_fsinfo(struct fat_fs *fs) { // making fsck happy
    if (fs->type != FAT_32)
        return; // no-op

    uint8_t *buf = kmalloc(fs->disk->sector_size, ALLOC_PARAMS_DEFAULT);
    if (!buf)
        return;

    if (!fs->disk->read_sector(fs->disk, fs->fsinfo_sector, buf, 1))
        return;

    *(uint32_t *) (buf + 0x1e8) = fs->free_clusters;
    *(uint32_t *) (buf + 0x1ec) = fs->last_alloc_cluster;

    fs->disk->write_sector(fs->disk, fs->fsinfo_sector, buf, 1);
    kfree(buf, FREE_PARAMS_DEFAULT);
}

uint32_t fat_first_data_sector(const struct fat_fs *fs) {
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t root_dir_sectors =
        ((bpb->root_entry_count * sizeof(struct fat_dirent)) +
         (bpb->bytes_per_sector - 1)) /
        bpb->bytes_per_sector;

    return bpb->reserved_sector_count + (bpb->num_fats * fs->fat_size) +
           ((fs->type != FAT_32) ? root_dir_sectors : 0);
}

uint32_t fat_cluster_to_lba(const struct fat_fs *fs, uint32_t cluster) {
    const struct fat_bpb *bpb = fs->bpb;

    if (cluster == FAT_DIR_CLUSTER_ROOT) {
        return fs->volume_base_lba + bpb->reserved_sector_count +
               (bpb->num_fats * fs->fat_size);
    }

    return (fat_first_data_sector(fs) +
            (cluster - 2) * bpb->sectors_per_cluster) +
           fs->volume_base_lba;
}

struct fat_bpb *fat_read_bpb(struct generic_disk *drive,
                             enum fat_fstype *out_type, uint32_t *out_lba,
                             uint32_t base_lba) {
    uint8_t *sector = kmalloc(drive->sector_size, ALLOC_PARAMS_DEFAULT);
    if (!sector)
        return NULL;

    uint32_t fat_lba = 0;

    if (base_lba != 0) {
        fat_lba = base_lba;
    } else {
        if (drive->read_sector(drive, 0, sector, 1)) {
            struct mbr *mbr = (struct mbr *) sector;
            if (mbr->signature == 0xAA55) {
                for (int i = 0; i < 4; i++) {
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

        enum fat_fstype type = 0xFF;

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

        if (type == 0xFF)
            continue;

        struct fat_bpb *out_bpb =
            kmalloc(sizeof(struct fat_bpb), ALLOC_PARAMS_DEFAULT);
        if (!out_bpb)
            return NULL;

        if (out_bpb) {
            memcpy(out_bpb, bpb, sizeof(struct fat_bpb));
            *out_type = type;
            *out_lba = lba;
            kfree(sector, FREE_PARAMS_DEFAULT);
            return out_bpb;
        }

        break;
    }

    kfree(sector, FREE_PARAMS_DEFAULT);
    return NULL;
}

struct vfs_node *fat_g_mount(struct generic_partition *p) {
    if (!p || !p->disk)
        return NULL;

    struct fat_fs *fs = kmalloc(sizeof(struct fat_fs), ALLOC_PARAMS_DEFAULT);
    if (!fs)
        return NULL;

    enum fat_fstype type;
    uint32_t lba;
    struct generic_disk *d = p->disk;
    struct fat_bpb *bpb = fat_read_bpb(d, &type, &lba, p->start_lba);
    if (!bpb) {
        kfree(fs, FREE_PARAMS_DEFAULT);
        return NULL;
    }

    fs->partition = p;
    fs->bpb = bpb;
    fs->type = type;
    fs->volume_base_lba = lba;
    fs->disk = d;

    struct fat32_ext_bpb f32_ext = bpb->ext_32;
    struct fat12_16_ext_bpb f16_ext = bpb->ext_12_16;

    bool f32 = type == FAT_32;

    fs->root_cluster = f32 ? f32_ext.root_cluster : FAT_DIR_CLUSTER_ROOT;
    fs->fat_size = f32 ? f32_ext.fat_size_32 : bpb->fat_size_16;
    fs->boot_signature = f32 ? f32_ext.boot_signature : f16_ext.boot_signature;
    fs->drive_number = f32 ? f32_ext.drive_number : f16_ext.drive_number;
    fs->volume_id = f32 ? f32_ext.volume_id : f16_ext.volume_id;
    fs->cluster_size = fs->bpb->sectors_per_cluster * fs->bpb->bytes_per_sector;
    fs->entries_per_cluster = fs->cluster_size / sizeof(struct fat_dirent);

    uint32_t total_sectors =
        f32 ? bpb->total_sectors_32 : bpb->total_sectors_16;

    uint32_t data_sectors = total_sectors - fat_first_data_sector(fs);
    fs->total_clusters = data_sectors / bpb->sectors_per_cluster;

    memcpy(fs->fs_type, f32 ? f32_ext.fs_type : f16_ext.fs_type, 8);
    memcpy(fs->volume_label, f32 ? f32_ext.volume_label : f16_ext.volume_label,
           11);

    if (f32) {
        uint16_t fsinfo_rel_sector = f32_ext.fs_info;
        uint8_t *buf = kmalloc(fs->disk->sector_size, ALLOC_PARAMS_DEFAULT);
        if (!buf)
            return NULL;

        if (!d->read_sector(d, fs->volume_base_lba + fsinfo_rel_sector, buf,
                            1)) {
            kfree(fs, FREE_PARAMS_DEFAULT);
            return NULL;
        }

        // TODO: #define these :boom:
        uint32_t lead_sig = *(uint32_t *) (buf + 0x00);
        uint32_t struc_sig = *(uint32_t *) (buf + 0x1fc);
        if (lead_sig != 0x41615252 || struc_sig != 0xAA550000) {
            kfree(fs, FREE_PARAMS_DEFAULT);
            return NULL;
        }

        fs->fsinfo_sector = fsinfo_rel_sector;
        fs->free_clusters = *(uint32_t *) (buf + 0x1e8);
        fs->last_alloc_cluster = *(uint32_t *) (buf + 0x1ec);

        if (fs->free_clusters == 0xFFFFFFFF)
            fs->free_clusters = 0; // unknown
        if (fs->last_alloc_cluster == 0xFFFFFFFF)
            fs->last_alloc_cluster = 2;
    } else {
        fs->fsinfo_sector = 0;
        fs->free_clusters = 0;
        fs->last_alloc_cluster = 0;
    }

    d->fs_data = fs;
    p->fs_data = fs;
    return NULL; // TODO: implement vfs here
}

void fat_g_print(struct generic_partition *d) {
    if (!d || !d->fs_data)
        return;

    struct fat_fs *fs = d->fs_data;

    switch (fs->type) {
    case FAT_12:
    case FAT_16: fat12_16_print_bpb(fs->bpb); break;
    case FAT_32: fat32_print_bpb(fs->bpb); break;
    }

    struct fat_dirent new_file_ent;

    bool success;

    fat_list_root(fs);

    success = fat_create(fs, fs->root_cluster, "Whimsy", &new_file_ent,
                         FAT_ARCHIVE, NULL);

    success = fat_delete(fs, fs->root_cluster, "Whimsy");

    success = fat_create(fs, fs->root_cluster, "Booh", &new_file_ent,
                         FAT_ARCHIVE, NULL);

    success = fat_create(fs, fs->root_cluster, "Cooh", &new_file_ent,
                         FAT_ARCHIVE, NULL);

    success = fat_create(fs, fs->root_cluster, "Dooh", &new_file_ent,
                         FAT_ARCHIVE, NULL);

    fat_list_root(fs);
    uint32_t ind;

    struct fat_dirent *f = fat_lookup(fs, fs->root_cluster, "Dooh", &ind);

    new_file_ent = f ? *f : new_file_ent;

    success = fat_write_file(fs, &new_file_ent, 0, (uint8_t *) "Doober\n", 8);

    success = fat_write_dirent(fs, fs->root_cluster, &new_file_ent, ind);

    if (success) {
        k_printf("yay\n");
    } else {
        k_printf("that not right...\n");
    }
    fat_list_root(fs);
}
