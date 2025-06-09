#include <fs/fat.h>

uint32_t fat_alloc_cluster(struct fat_fs *fs) {
    uint32_t total_clusters = fs->total_clusters;
    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        uint32_t entry = fat_read_fat_entry(fs, cluster);
        if (entry == 0x00000000) {
            fat_write_fat_entry(fs, cluster, fat_eoc(fs));
            return cluster;
        }
    }
    return 0;
}

void fat_free_chain(struct fat_fs *fs, uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    while (!fat_is_eoc(fs, cluster)) {
        uint32_t next = fat_read_fat_entry(fs, cluster);
        fat_write_fat_entry(fs, cluster, 0x00000000);
        if (next == cluster)
            return;
        cluster = next;
    }
    fat_write_fat_entry(fs, cluster, 0x00000000);
}
