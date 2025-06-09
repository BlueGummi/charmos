#include <fs/fat.h>

uint32_t fat_first_data_sector(const struct fat_fs *fs) {
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t root_dir_sectors =
        ((bpb->root_entry_count * 32) + (bpb->bytes_per_sector - 1)) /
        bpb->bytes_per_sector;

    uint32_t fat_size =
        (fs->type == FAT_32) ? bpb->ext_32.fat_size_32 : bpb->fat_size_16;

    return bpb->reserved_sector_count + (bpb->num_fats * fat_size) +
           ((fs->type == FAT_12 || fs->type == FAT_16) ? root_dir_sectors : 0);
}

uint32_t fat_cluster_to_lba(const struct fat_fs *fs, uint32_t cluster) {
    const struct fat_bpb *bpb = fs->bpb;
    return fat_first_data_sector(fs) + (cluster - 2) * bpb->sectors_per_cluster;
}

bool fat_read_cluster(struct generic_disk *disk, uint32_t cluster,
                      uint8_t *buffer) {
    struct fat_fs *fs = disk->fs_data;
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    return disk->read_sector(disk, lba, buffer, bpb->sectors_per_cluster);
}
