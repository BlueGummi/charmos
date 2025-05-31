#include <mem/alloc.h>
#include <fs/ext2.h>
#include <console/printf.h>
#include <stdint.h>

static bool walk_dir(struct ext2_fs *fs, uint32_t block_num,
                     dir_entry_callback callback, void *ctx) {
    uint8_t *dir_buf = kmalloc(fs->block_size);
    if (!dir_buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    if (!ext2_block_read(fs->drive, lba, dir_buf, fs->sectors_per_block)) {
        kfree(dir_buf);
        return false;
    }

    uint32_t offset = 0;
    bool modified = false;

    while (offset < fs->block_size) {
        struct ext2_dir_entry *entry =
            (struct ext2_dir_entry *) (dir_buf + offset);
        if (entry->rec_len < 8 || offset + entry->rec_len > fs->block_size)
            break;

        if (callback(fs, entry, ctx, block_num, entry->inode, offset)) {
            modified = true;
            break;
        }

        offset += entry->rec_len;
    }

    if (modified) {
        ext2_block_write(fs->drive, lba, dir_buf, fs->sectors_per_block);
    }

    kfree(dir_buf);
    return modified;
}

static bool walk_direct_blocks(struct ext2_fs *fs, struct k_full_inode *inode,
                               dir_entry_callback cb, void *ctx,
                               bool ff_avail) {
    for (uint32_t i = 0; i < 12; ++i) {
        if (!inode->node.block[i] && ff_avail) {
            inode->node.block[i] = *(uint32_t *) ctx;
            inode->node.blocks += fs->block_size / fs->drive->sector_size;
            inode->node.size += fs->block_size;
            ext2_write_inode(fs, inode->node.block[i], &inode->node);
            return true;
        }

        if (inode->node.block[i] && walk_dir(fs, inode->node.block[i], cb, ctx))
            return true;
    }
    return false;
}

static bool walk_single(struct ext2_fs *fs, dir_entry_callback cb, void *ctx,
                        bool ff_avail, struct k_full_inode *inode) {
    uint32_t block_num = inode->node.block[12];
    uint32_t ptrs[PTRS_PER_BLOCK];
    if (!ext2_block_ptr_read(fs, block_num, ptrs)) {
        return false;
    }

    for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
        if (!ptrs[i] && ff_avail) {
            ptrs[i] = *(uint32_t *) ctx;
            inode->node.blocks += fs->block_size / fs->drive->sector_size;
            inode->node.size += fs->block_size;
            if (!ext2_block_ptr_write(fs, block_num, ptrs))
                return false;
            return true;
        }

        if (ptrs[i] && walk_dir(fs, ptrs[i], cb, ctx))
            return true;
    }
    return false;
}

static bool walk_double(struct ext2_fs *fs, dir_entry_callback cb, void *ctx,
                        bool ff_avail, struct k_full_inode *inode) {
    uint32_t block_num = inode->node.block[13];
    uint32_t level1[PTRS_PER_BLOCK];
    if (!ext2_block_ptr_read(fs, block_num, level1))
        return false;

    for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
        if (!level1[i] && ff_avail) {
            level1[i] = *(uint32_t *) ctx;
            inode->node.blocks += fs->block_size / fs->drive->sector_size;
            inode->node.size += fs->block_size;
            if (!ext2_block_ptr_write(fs, block_num, level1))
                return false;
            return true;
        }

        if (!level1[i])
            continue;

        uint32_t level2[PTRS_PER_BLOCK];
        if (!ext2_block_ptr_read(fs, level1[i], level2))
            continue;

        for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
            if (!level2[j] && ff_avail) {
                level2[j] = *(uint32_t *) ctx;
                inode->node.blocks += fs->block_size / fs->drive->sector_size;
                inode->node.size += fs->block_size;
                if (!ext2_block_ptr_write(fs, level1[i], level2))
                    return false;
                return true;
            }

            if (level2[j] && walk_dir(fs, level2[j], cb, ctx))
                return true;
        }
    }
    return false;
}

static bool walk_triple(struct ext2_fs *fs, dir_entry_callback cb, void *ctx,
                        bool ff_avail, struct k_full_inode *inode) {
    uint32_t block_num = inode->node.block[14];
    uint32_t level1[PTRS_PER_BLOCK];
    if (!ext2_block_ptr_read(fs, block_num, level1))
        return false;

    for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
        if (!level1[i] && ff_avail) {
            level1[i] = *(uint32_t *) ctx;
            inode->node.blocks += fs->block_size / fs->drive->sector_size;
            inode->node.size += fs->block_size;
            if (!ext2_block_ptr_write(fs, block_num, level1))
                return false;
            return true;
        }

        if (!level1[i])
            continue;

        uint32_t level2[PTRS_PER_BLOCK];
        if (!ext2_block_ptr_read(fs, level1[i], level2))
            continue;

        for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
            if (!level2[j] && ff_avail) {
                level2[j] = *(uint32_t *) ctx;
                inode->node.blocks += fs->block_size / fs->drive->sector_size;
                inode->node.size += fs->block_size;
                if (!ext2_block_ptr_write(fs, level1[i], level2))
                    return false;
                return true;
            }

            if (!level2[j])
                continue;

            uint32_t level3[PTRS_PER_BLOCK];
            if (!ext2_block_ptr_read(fs, level2[j], level3))
                continue;

            for (uint32_t k = 0; k < PTRS_PER_BLOCK; ++k) {
                if (!level3[k] && ff_avail) {
                    level3[k] = *(uint32_t *) ctx;
                    inode->node.blocks +=
                        fs->block_size / fs->drive->sector_size;
                    inode->node.size += fs->block_size;
                    if (!ext2_block_ptr_write(fs, level2[j], level3))
                        return false;
                    return true;
                }

                if (level3[k] && walk_dir(fs, level3[k], cb, ctx))
                    return true;
            }
        }
    }
    return false;
}

bool ext2_walk_dir(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                   dir_entry_callback cb, void *ctx, bool ff_avail) {
    if (!fs || !dir_inode || !cb)
        return false;

    if (walk_direct_blocks(fs, dir_inode, cb, ctx, ff_avail))
        return true;

    if (dir_inode->node.block[12] &&
        walk_single(fs, cb, ctx, ff_avail, dir_inode))
        return true;

    if (dir_inode->node.block[13] &&
        walk_double(fs, cb, ctx, ff_avail, dir_inode))
        return true;

    if (dir_inode->node.block[14] &&
        walk_triple(fs, cb, ctx, ff_avail, dir_inode))
        return true;

    return false;
}

static void traverse_indirect(struct ext2_fs *fs, struct ext2_inode *inode,
                              uint32_t block_num, int depth,
                              ext2_block_visitor visitor, void *user_data) {
    if (depth <= 0 || block_num == 0)
        return;

    uint32_t *block = kmalloc(fs->block_size);
    if (!block)
        return;

    ext2_block_ptr_read(fs, block_num, (uint8_t *) block);

    for (uint32_t i = 0; i < fs->block_size / sizeof(uint32_t); i++) {
        if (block[i] || depth == 1) {
            visitor(fs, inode, depth, &block[i], user_data);
        }

        if (depth > 1 && block[i]) {
            traverse_indirect(fs, inode, block[i], depth - 1, visitor,
                              user_data);
        }
    }

    ext2_block_ptr_write(fs, block_num, (uint8_t *) block);
    kfree(block);
}

void ext2_traverse_inode_blocks(struct ext2_fs *fs, struct ext2_inode *inode,
                                ext2_block_visitor visitor, void *user_data) {
    for (int i = 0; i < 12; i++) {
        visitor(fs, inode, 0, &inode->block[i], user_data);
    }

    if (inode->block[12])
        traverse_indirect(fs, inode, inode->block[12], 1, visitor, user_data);

    if (inode->block[13])
        traverse_indirect(fs, inode, inode->block[13], 2, visitor, user_data);

    if (inode->block[14])
        traverse_indirect(fs, inode, inode->block[14], 3, visitor, user_data);
}
