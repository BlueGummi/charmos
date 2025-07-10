#include <fs/ext2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint32_t ext2_get_block(struct ext2_fs *fs, uint32_t block_num,
                               uint32_t depth, uint32_t block_index,
                               uint32_t new_block_num, bool allocate,
                               bool *was_allocated) {
    if (depth == 0) {
        // actual data block
        return block_num;
    }

    uint32_t pointers_per_block = fs->block_size / sizeof(uint32_t);

    bool allocated_this_level = false;
    struct bcache_entry *ent;
    uint32_t *block = (uint32_t *) ext2_block_read(fs, block_num, &ent);

    if (block_num == 0) {
        if (!allocate) {
            return 0;
        }

        block_num = ext2_alloc_block(fs);
        if (block_num == 0) {
            return 0;
        }

        allocated_this_level = true;

        block = (uint32_t *) ext2_block_read(fs, block_num, &ent);
    }

    bool i = bcache_ent_lock(ent);

    if (allocated_this_level) {
        memset(block, 0, fs->block_size);
    }

    uint32_t index = block_index;
    uint32_t divisor = 1;
    for (uint32_t i = 1; i < depth; ++i)
        divisor *= pointers_per_block;

    uint32_t entry_index = index / divisor;
    uint32_t entry_offset = index % divisor;
    uint32_t bnum = block[entry_index];
    bcache_ent_unlock(ent, i);

    uint32_t result;
    result = ext2_get_block(fs, bnum, depth - 1, entry_offset, new_block_num,
                            allocate, was_allocated);
    i = bcache_ent_lock(ent);

    if (result && block[entry_index] == 0 && allocate) {
        block[entry_index] = result;
        bcache_ent_unlock(ent, i);

        ext2_block_write(fs, ent, EXT2_PRIO_DIRENT);

        bcache_ent_lock(ent);
    } else if (allocated_this_level && result == 0) {
        ext2_free_block(fs, block_num);
    }

    bcache_ent_unlock(ent, i);

    return result;
}

uint32_t ext2_get_or_set_block(struct ext2_fs *fs, struct ext2_inode *inode,
                               uint32_t block_index, uint32_t new_block_num,
                               bool allocate, bool *was_allocated) {
    if (!fs || !inode)
        return (uint32_t) -1;

    if (was_allocated)
        *was_allocated = false;

    uint32_t pointers_per_block = fs->block_size / sizeof(uint32_t);

    if (block_index < 12) {
        if (inode->block[block_index] == 0 && allocate) {
            if (new_block_num == 0)
                new_block_num = ext2_alloc_block(fs);

            if (new_block_num == 0)
                return 0;

            inode->block[block_index] = new_block_num;
            if (was_allocated)
                *was_allocated = true;
        }
        return inode->block[block_index];
    }

    block_index -= 12;

    if (block_index < pointers_per_block) {
        return ext2_get_block(fs, inode->block[12], 1, block_index,
                              new_block_num, allocate, was_allocated);
    }

    block_index -= pointers_per_block;

    if (block_index < pointers_per_block * pointers_per_block) {
        return ext2_get_block(fs, inode->block[13], 2, block_index,
                              new_block_num, allocate, was_allocated);
    }

    block_index -= pointers_per_block * pointers_per_block;

    uint64_t max_triple =
        (uint64_t) pointers_per_block * pointers_per_block * pointers_per_block;
    if ((uint64_t) block_index < max_triple) {
        return ext2_get_block(fs, inode->block[14], 3, block_index,
                              new_block_num, allocate, was_allocated);
    }

    return (uint32_t) -1;
}

void ext2_init_inode(struct ext2_inode *new_inode, uint16_t mode) {
    new_inode->mode = mode;
    new_inode->uid = 0;
    new_inode->gid = 0; // TODO: these
    new_inode->size = 0;
    new_inode->atime = time_get_unix();
    new_inode->ctime = new_inode->mtime = new_inode->atime;
    new_inode->links_count = ((mode & EXT2_S_IFDIR) ? 2 : 1);
    new_inode->blocks = 0;
    new_inode->flags = 0;
}

void ext2_init_dirent(struct ext2_fs *fs, struct ext2_dir_entry *new_entry,
                      uint32_t inode_num, const char *name, uint8_t type) {
    new_entry->inode = inode_num;
    new_entry->name_len = strlen(name);
    new_entry->rec_len = fs->block_size;
    new_entry->file_type = type;
    memcpy(new_entry->name, name, new_entry->name_len);
}

uint8_t ext2_extract_ftype(uint16_t mode) {
    uint8_t file_type;
    if (mode & EXT2_S_IFDIR)
        file_type = EXT2_FT_DIR;
    else if (mode & EXT2_S_IFREG)
        file_type = EXT2_FT_REG_FILE;
    else if (mode & EXT2_S_IFLNK)
        file_type = EXT2_FT_SYMLINK;
    else
        file_type = EXT2_FT_UNKNOWN;

    return file_type;
}

uint32_t ext2_get_block_group(struct ext2_fs *fs, uint32_t block) {
    return (block - 1) / fs->sblock->blocks_per_group;
}

uint32_t ext2_get_inode_group(struct ext2_fs *fs, uint32_t inode) {
    return (inode - 1) / fs->sblock->inodes_per_group;
}

bool ext2_fs_lock(struct ext2_fs *fs) {
    return spin_lock(&fs->lock);
}

void ext2_fs_unlock(struct ext2_fs *fs, bool b) {
    spin_unlock(&fs->lock, b);
}

void ext2_prefetch_block(struct ext2_fs *fs, uint32_t block) {
    uint32_t lba = ext2_block_to_lba(fs, block);
    bcache_prefetch_async(fs->drive, lba, fs->block_size,
                          fs->sectors_per_block);
}

uint8_t *ext2_create_bcache_ent(struct ext2_fs *fs, uint32_t block,
                                struct bcache_entry **out) {
    return bcache_create_ent(fs->drive, ext2_block_to_lba(fs, block),
                             fs->block_size, fs->sectors_per_block, false, out);
}
