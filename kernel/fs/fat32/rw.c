#include <fs/fat.h>

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
