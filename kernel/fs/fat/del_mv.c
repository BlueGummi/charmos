#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

// TODO: errno

bool fat_delete(struct fat_fs *fs, uint32_t dir_cluster, const char *filename) {
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

    kfree(dirent);

    return true;
}

bool fat_rename(struct fat_fs *fs, uint32_t dir_cluster, const char *filename,
                const char *new_filename) {
    char fmtname[11] = {0};
    fat_format_filename_83(new_filename, fmtname);
    uint32_t index;
    struct fat_dirent *dirent = fat_lookup(fs, dir_cluster, filename, &index);

    if (!dirent)
        return false;

    if (fat_contains(fs, dir_cluster, new_filename))
        return false;

    memcpy(dirent->name, fmtname, 11);

    if (!fat_write_dirent(fs, dir_cluster, dirent, index)) {
        return false;
    }

    kfree(dirent);

    return true;
}
