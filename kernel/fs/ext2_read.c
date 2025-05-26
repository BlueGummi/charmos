#include <disk.h>
#include <fs/ext2.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>

bool block_read(struct ide_drive *d, uint32_t lba, uint8_t *buffer, uint32_t sector_count) {
    if (!buffer)
        return false;

    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!ide_read_sector(d, lba + i, buffer + (i * 512))) {
            return false;
        }
    }
    return true;
}

bool read_block_ptrs(struct ext2_fs *fs, uint32_t block_num, uint32_t *buf) {
    if (!fs || !buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    return block_read(fs->drive, lba, (uint8_t *) buf, fs->sectors_per_block);
}

bool ext2_read_inode(struct ext2_fs *fs, uint32_t inode_idx,
                     struct ext2_inode *inode_out) {
    if (!fs || !inode_out || inode_idx == 0)
        return false;

    uint32_t inodes_per_group = fs->sblock->inodes_per_group;
    uint32_t inode_size = fs->sblock->inode_size;

    uint32_t group = (inode_idx - 1) / inodes_per_group;
    uint32_t index_in_group = (inode_idx - 1) % inodes_per_group;

    struct ext2_group_desc *desc = &fs->group_desc[group];
    uint32_t inode_table_block = desc->inode_table;

    uint32_t offset_bytes = index_in_group * inode_size;
    uint32_t block_offset = offset_bytes / fs->block_size;
    uint32_t offset_in_block = offset_bytes % fs->block_size;

    uint32_t inode_block_num = inode_table_block + block_offset;
    uint32_t inode_lba = inode_block_num * fs->sectors_per_block;

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf)
        return false;

    if (!block_read(fs->drive, inode_lba, buf, fs->sectors_per_block)) {
        kfree(buf, fs->block_size);
        return false;
    }

    memcpy(inode_out, buf + offset_in_block, sizeof(struct ext2_inode));
    kfree(buf, fs->block_size);
    return true;
}

static bool search_dir_block(struct ext2_fs *fs, uint32_t block_num,
                             const char *fname, struct ext2_inode **out_node) {
    if (!fs || !fname || !out_node) {
        return false;
    }

    uint8_t dir_buf[fs->block_size];
    uint32_t dir_lba = block_num * fs->sectors_per_block;

    if (!block_read(fs->drive, dir_lba, dir_buf, fs->sectors_per_block))
        return false;

    uint32_t offset = 0;
    while (offset < fs->block_size) {
        struct ext2_dir_entry *entry =
            (struct ext2_dir_entry *) (dir_buf + offset);

        if (entry->rec_len < 8 || entry->rec_len + offset > fs->block_size)
            break;

        if (entry->inode != 0 &&
            memcmp(entry->name, fname, entry->name_len) == 0 &&
            fname[entry->name_len] == '\0') {

            *out_node = kmalloc(sizeof(struct ext2_inode));
            ext2_read_inode(fs, entry->inode, *out_node);
            return true;
        }

        offset += entry->rec_len;
    }

    return false;
}

struct ext2_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                         const struct ext2_inode *dir_inode,
                                         const char *fname) {
    if (!fs || !dir_inode || !fname) {
        return NULL;
    }

    struct ext2_inode *found = NULL;

    // Direct blocks (0..11)
    for (uint32_t i = 0; i < 12; ++i) {
        if (dir_inode->block[i] != 0 &&
            search_dir_block(fs, dir_inode->block[i], fname, &found))
            return found;
    }

    // Singly indirect
    if (dir_inode->block[12]) {
        uint32_t ptrs[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[12], ptrs)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (ptrs[i] != 0 &&
                    search_dir_block(fs, ptrs[i], fname, &found))
                    return found;
            }
        }
    }

    // Doubly indirect
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
                            search_dir_block(fs, level2[j], fname, &found))
                            return found;
                    }
                }
            }
        }
    }

    // Triply indirect
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
                                    search_dir_block(fs, level3[k], fname,
                                                     &found))
                                    return found;
                            }
                        }
                    }
                }
            }
        }
    }

    return NULL;
}
