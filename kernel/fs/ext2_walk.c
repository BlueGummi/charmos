#include <fs/ext2.h>
#include <vmalloc.h>

static bool walk_dir_block(struct ext2_fs *fs, uint32_t block_num,
                           bool (*callback)(struct ext2_fs *,
                                            struct ext2_dir_entry *, void *),
                           void *ctx) {
    uint8_t *dir_buf = kmalloc(fs->block_size);
    if (!dir_buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    if (!block_read(fs->drive, lba, dir_buf, fs->sectors_per_block)) {
        kfree(dir_buf, fs->block_size);
        return false;
    }

    uint32_t offset = 0;
    bool modified = false;

    while (offset < fs->block_size) {
        struct ext2_dir_entry *entry =
            (struct ext2_dir_entry *) (dir_buf + offset);
        if (entry->rec_len < 8 || offset + entry->rec_len > fs->block_size)
            break;

        if (callback(fs, entry, ctx)) {
            modified = true;
            break;
        }

        offset += entry->rec_len;
    }

    if (modified) {
        block_write(fs->drive, lba, dir_buf, fs->sectors_per_block);
    }

    kfree(dir_buf, fs->block_size);
    return modified;
}

bool walk_directory_entries(struct ext2_fs *fs,
                            const struct ext2_inode *dir_inode,
                            dir_entry_callback cb, void *ctx) {
    if (!fs || !dir_inode || !cb)
        return false;

    for (uint32_t i = 0; i < 12; ++i) {
        if (dir_inode->block[i] != 0 &&
            walk_dir_block(fs, dir_inode->block[i], cb, ctx))
            return true;
    }

    if (dir_inode->block[12]) {
        uint32_t ptrs[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[12], ptrs)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (ptrs[i] != 0 && walk_dir_block(fs, ptrs[i], cb, ctx))
                    return true;
            }
        }
    }

    if (dir_inode->block[13]) {
        uint32_t level1[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[13], level1)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (level1[i] == 0)
                    continue;

                uint32_t level2[PTRS_PER_BLOCK];
                if (read_block_ptrs(fs, level1[i], level2)) {
                    for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                        if (level2[j] != 0 &&
                            walk_dir_block(fs, level2[j], cb, ctx))
                            return true;
                    }
                }
            }
        }
    }

    if (dir_inode->block[14]) {
        uint32_t level1[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[14], level1)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (level1[i] == 0)
                    continue;

                uint32_t level2[PTRS_PER_BLOCK];
                if (read_block_ptrs(fs, level1[i], level2)) {
                    for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                        if (level2[j] == 0)
                            continue;

                        uint32_t level3[PTRS_PER_BLOCK];
                        if (read_block_ptrs(fs, level2[j], level3)) {
                            for (uint32_t k = 0; k < PTRS_PER_BLOCK; ++k) {
                                if (level3[k] != 0 &&
                                    walk_dir_block(fs, level3[k], cb, ctx))
                                    return true;
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}
