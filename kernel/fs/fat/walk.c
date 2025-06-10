#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

// TODO: optimize this so I dont memcpy every iteration :boom:

static bool fat32_walk_cluster(struct fat_fs *fs, uint32_t cluster,
                               fat_walk_callback callback, void *ctx) {
    uint8_t *cluster_buf = kmalloc(fs->cluster_size);

    struct fat_dirent *ret = kmalloc(sizeof(struct fat_dirent));
    if (fat_read_cluster(fs, cluster, cluster_buf)) {
        for (uint32_t i = 0; i < fs->cluster_size;
             i += sizeof(struct fat_dirent)) {
            struct fat_dirent *entry = (struct fat_dirent *) (cluster_buf + i);
            memcpy(ret, entry, sizeof(struct fat_dirent));
            if (callback(ret, i / sizeof(struct fat_dirent), ctx)) {
                kfree(cluster_buf);
                return true;
            }
        }
    }
    kfree(ret);
    kfree(cluster_buf);
    return false;
}

static bool fat12_16_walk_cluster(struct fat_fs *fs, uint32_t cluster,
                                  fat_walk_callback callback, void *ctx) {
    struct fat_bpb *bpb = fs->bpb;

    uint32_t sectors_per_cluster = fs->bpb->sectors_per_cluster;
    bool is_root = cluster == FAT_DIR_CLUSTER_ROOT;

    uint32_t root_dir_size = bpb->root_entry_count * sizeof(struct fat_dirent);

    uint32_t root_dir_sectors =
        (root_dir_size + bpb->bytes_per_sector - 1) / bpb->bytes_per_sector;

    uint32_t root_start_lba = fs->volume_base_lba + bpb->reserved_sector_count +
                              (bpb->num_fats * fs->fat_size);

    uint32_t lba = is_root ? root_start_lba : fat_cluster_to_lba(fs, cluster);
    uint32_t sectors_to_read = is_root ? root_dir_sectors : sectors_per_cluster;

    uint8_t *sector_buf = kmalloc(sectors_to_read * bpb->bytes_per_sector);
    if (!sector_buf)
        return false;

    if (!fs->disk->read_sector(fs->disk, lba, sector_buf, sectors_to_read)) {
        kfree(sector_buf);
        return false;
    }

    struct fat_dirent *ret = kmalloc(sizeof(struct fat_dirent));
    uint32_t entries_per_cluster =
        is_root ? bpb->root_entry_count
                : (sectors_to_read * bpb->bytes_per_sector) /
                      sizeof(struct fat_dirent);

    for (uint32_t i = 0; i < entries_per_cluster; i++) {
        struct fat_dirent *entry =
            (struct fat_dirent *) (sector_buf + i * sizeof(struct fat_dirent));
        memcpy(ret, entry, sizeof(struct fat_dirent));
        if (callback(ret, i / sizeof(struct fat_dirent), ctx)) {
            kfree(sector_buf);
            return true;
        }
    }

    kfree(ret);
    kfree(sector_buf);
    return false;
}

// If the callback returns true, the dirent passed in will be allocated
bool fat_walk_cluster(struct fat_fs *fs, uint32_t cluster, fat_walk_callback cb,
                      void *ctx) {
    if (fs->type == FAT_32) {
        return fat32_walk_cluster(fs, cluster, cb, ctx);
    } else {
        return fat12_16_walk_cluster(fs, cluster, cb, ctx);
    }
}
