#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

bool fat_read_file(struct fat_fs *fs, struct fat_dirent *ent, uint32_t offset,
                   uint32_t size, uint8_t *out_buf) {
    if (!ent || !out_buf || offset >= ent->filesize)
        return false;

    uint32_t cluster_size = fs->cluster_size;
    uint32_t file_size = ent->filesize;

    // Do not read past end of file
    if (offset + size > file_size)
        size = file_size - offset;

    uint32_t cluster = fat_get_dir_cluster(ent);
    if (cluster == 0)
        return false;

    uint8_t *temp_buf = kmalloc(cluster_size);
    uint32_t read = 0;

    // reach the starting offset
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t skip_offset = offset % cluster_size;

    while (skip_clusters--) {
        if (fat_is_eoc(fs, cluster))
            goto oops;

        cluster = fat_read_fat_entry(fs, cluster);
    }

    while (read < size && !fat_is_eoc(fs, cluster)) {
        if (!fat_read_cluster(fs, cluster, temp_buf))
            goto oops;

        uint32_t to_copy = cluster_size - skip_offset;
        if (to_copy > (size - read))
            to_copy = size - read;

        memcpy(out_buf + read, temp_buf + skip_offset, to_copy);
        read += to_copy;

        skip_offset = 0;
        if (read < size)
            cluster = fat_read_fat_entry(fs, cluster);
    }

    kfree(temp_buf);
    return true;
oops:
    kfree(temp_buf);
    return false;
}

bool fat_write_file(struct fat_fs *fs, struct fat_dirent *ent, uint32_t offset,
                    const uint8_t *data, uint32_t size) {
    if (!ent || !data)
        return false;

    uint32_t cluster_size = fs->cluster_size;
    uint32_t cluster = fat_get_dir_cluster(ent);
    if (cluster == 0) {
        cluster = fat_alloc_cluster(fs);
        if (cluster == 0)
            return false;

        ent->high_cluster = cluster >> 16;
        ent->low_cluster = cluster & 0xFFFF;

        fat_write_fat_entry(fs, cluster, fat_eoc(fs));
    }

    uint32_t end_offset = offset + size;
    uint32_t needed_clusters = (end_offset + cluster_size - 1) / cluster_size;

    // extend chain to needed length
    uint32_t chain_len = 1;
    uint32_t current = cluster;
    while (!fat_is_eoc(fs, current)) {
        current = fat_read_fat_entry(fs, current);
        chain_len++;
    }

    current = cluster;
    if (chain_len < needed_clusters) {
        for (uint32_t i = chain_len; i < needed_clusters; i++) {
            uint32_t new_cluster = fat_alloc_cluster(fs);
            if (!new_cluster)
                return false;

            fat_write_fat_entry(fs, current, new_cluster);
            current = new_cluster;
        }
        fat_write_fat_entry(fs, current, fat_eoc(fs));
    }

    // Reset to start and walk to offset
    current = cluster;
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t skip_offset = offset % cluster_size;
    for (uint32_t i = 0; i < skip_clusters; i++) {
        current = fat_read_fat_entry(fs, current);
    }

    uint8_t *temp_buf = kmalloc(cluster_size);
    uint32_t written = 0;

    while (written < size && !fat_is_eoc(fs, current)) {
        if (skip_offset || (size - written) < cluster_size) {
            // Partial write — read-modify-write
            if (!fat_read_cluster(fs, current, temp_buf))
                goto oops;
        }

        uint32_t to_write = cluster_size - skip_offset;
        if (to_write > (size - written))
            to_write = size - written;

        memcpy(temp_buf + skip_offset, data + written, to_write);

        if (!fat_write_cluster(fs, current, temp_buf))
            goto oops;

        written += to_write;
        skip_offset = 0;

        if (written < size)
            current = fat_read_fat_entry(fs, current);
    }

    if (end_offset > ent->filesize)
        ent->filesize = end_offset;

    kfree(temp_buf);
    return true;
oops:
    kfree(temp_buf);
    return false;
}
