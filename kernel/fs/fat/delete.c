#include <console/printf.h>
#include <fs/fat.h>

bool fat_delete_file(struct fat_fs *fs, uint32_t dir_cluster,
                     const char *filename) {
    uint32_t index;
    struct fat_dirent *dirent = fat_lookup(fs, dir_cluster, filename, &index);
    if (!dirent)
        return false;

    uint32_t start_cluster = fat_get_dir_cluster(dirent);
    ((uint8_t *) dirent->name)[0] = 0xE5;

    if (!fat_write_dirent(fs, dir_cluster, dirent, index)) {
        return false;
    }

    fat_free_chain(fs, start_cluster);

    return true;
}
