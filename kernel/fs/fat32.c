#include <disk.h>
#include <fs/fat32.h>
#include <fs/fat32_print.h>
#include <printf.h>
#include <string.h>

void fat32_ls(struct ide_drive *drive) {
    struct fat32_bpb bpb;
    uint8_t sector[512];

    if (!ide_read_sector(drive, 0, sector)) {
        k_printf("Failed to read boot sector.\n");
        return;
    }

    memcpy(&bpb, sector, sizeof(struct fat32_bpb));
    fat32_print_bpb(&bpb);

    uint32_t fat_start = bpb.reserved_sector_count;
    uint32_t fat_sectors = bpb.fat_size_32;
    uint32_t data_start = fat_start + (bpb.num_fats * fat_sectors);
    //    uint32_t cluster_size = bpb.bytes_per_sector *
    //    bpb.sectors_per_cluster;
    uint32_t root_cluster = bpb.root_cluster;

    k_printf("Root dir cluster: %u\n", root_cluster);

    for (uint8_t s = 0; s < bpb.sectors_per_cluster; s++) {
        uint32_t lba =
            data_start + ((root_cluster - 2) * bpb.sectors_per_cluster) + s;

        if (!ide_read_sector(drive, lba, sector)) {
            k_printf("Failed to read root directory sector at LBA %u\n", lba);
            return;
        }

        for (uint32_t i = 0;
             i < bpb.bytes_per_sector / sizeof(struct fat_dirent); ++i) {
            struct fat_dirent *entry =
                (struct fat_dirent *) (sector + i * sizeof(struct fat_dirent));
            fat32_print_dirent(entry);
        }
    }
}
