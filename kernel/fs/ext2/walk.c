#include <fs/ext2.h>
#include <stdbool.h>
#include <stdint.h>

static bool walk_dir(struct ext2_fs *fs, uint32_t block_num,
                     dir_entry_callback callback, void *ctx) {

    struct bcache_entry *ent;
    uint8_t *dir_buf = ext2_block_read(fs, block_num, &ent);
    if (!dir_buf)
        return false;

    bcache_ent_acquire(ent);

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

    bcache_ent_release(ent);

    if (modified)
        ext2_block_write(fs, ent, EXT2_PRIO_DIRENT);

    return modified;
}

static bool walk_direct_blocks(struct ext2_fs *fs,
                               struct ext2_full_inode *inode,
                               dir_entry_callback cb, void *ctx,
                               bool ff_avail) {
    for (uint32_t i = 0; i < 12; ++i) {
        if (!inode->node.block[i] && ff_avail) {
            inode->node.block[i] = *(uint32_t *) ctx;
            inode->node.blocks += fs->block_size / fs->drive->sector_size;
            inode->node.size += fs->block_size;
            ext2_inode_write(fs, inode->node.block[i], &inode->node);
            return true;
        }

        if (inode->node.block[i] && walk_dir(fs, inode->node.block[i], cb, ctx))
            return true;
    }
    return false;
}

static bool walk_indirect(struct ext2_fs *fs, uint32_t block_num, int level,
                          dir_entry_callback cb, void *ctx, bool ff_avail,
                          struct ext2_full_inode *inode) {
    if (block_num == 0)
        return false;

    struct bcache_entry *ent;
    uint32_t *ptrs = (uint32_t *) ext2_block_read(fs, block_num, &ent);
    if (!ptrs)
        return false;

    bcache_ent_acquire(ent);

    for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
        if (!ptrs[i] && ff_avail) {

            ptrs[i] = *(uint32_t *) ctx;

            inode->node.blocks += fs->block_size / fs->drive->sector_size;
            inode->node.size += fs->block_size;

            bcache_ent_release(ent);
            if (!ext2_block_write(fs, ent, EXT2_PRIO_DIRENT))
                return false;
            return true;
        }

        if (!ptrs[i])
            continue;

        if (level == 1) {
            if (walk_dir(fs, ptrs[i], cb, ctx)) {
                bcache_ent_release(ent);
                return true;
            }
        } else {
            if (walk_indirect(fs, ptrs[i], level - 1, cb, ctx, ff_avail,
                              inode)) {
                bcache_ent_release(ent);
                return true;
            }
        }
    }

    bcache_ent_release(ent);
    return false;
}

static bool do_walk_dir(struct ext2_fs *fs, struct ext2_full_inode *dir_inode,
                        dir_entry_callback cb, void *ctx, bool ff_avail) {
    if (!fs || !dir_inode || !cb)
        return false;

    if (walk_direct_blocks(fs, dir_inode, cb, ctx, ff_avail))
        return true;

    if (walk_indirect(fs, dir_inode->node.block[12], 1, cb, ctx, ff_avail,
                      dir_inode))
        return true;

    if (walk_indirect(fs, dir_inode->node.block[13], 2, cb, ctx, ff_avail,
                      dir_inode))
        return true;

    if (walk_indirect(fs, dir_inode->node.block[14], 3, cb, ctx, ff_avail,
                      dir_inode))
        return true;

    return false;
}

static void readahead_blocks(struct ext2_fs *fs, uint32_t *entries,
                             uint32_t count, uint32_t start_index) {
    for (uint32_t j = 1; j <= 2 && (start_index + j) < count; j++) {
        uint32_t next = entries[start_index + j];
        if (next)
            ext2_prefetch_block(fs, next);
    }
}

static void traverse_indirect(struct ext2_fs *fs, struct ext2_inode *inode,
                              uint32_t block_num, int depth,
                              ext2_block_visitor visitor, void *user_data,
                              bool readahead) {
    if (depth <= 0 || block_num == 0)
        return;

    struct bcache_entry *ent;
    uint32_t *block = (uint32_t *) ext2_block_read(fs, block_num, &ent);
    if (!block)
        return;

    bcache_ent_acquire(ent);
    uint32_t size = sizeof(uint32_t);

    for (uint32_t i = 0; i < fs->block_size / sizeof(uint32_t); i++) {
        if (block[i] || depth == 1) {
            visitor(fs, inode, depth, &block[i], user_data);
        }

        if (depth > 1 && block[i]) {
            if (readahead)
                readahead_blocks(fs, block, fs->block_size / size, i);

            traverse_indirect(fs, inode, block[i], depth - 1, visitor,
                              user_data, readahead);
        }
    }

    bcache_ent_release(ent);
    ext2_block_write(fs, ent, EXT2_PRIO_DIRENT);
}

void ext2_traverse_inode_blocks(struct ext2_fs *fs, struct ext2_inode *inode,
                                ext2_block_visitor visitor, void *user_data,
                                bool readahead) {
    for (int i = 0; i < 12; i++) {
        if (readahead && inode->block[i])
            ext2_prefetch_block(fs, inode->block[i]);

        visitor(fs, inode, 0, &inode->block[i], user_data);
    }

    if (inode->block[12])
        traverse_indirect(fs, inode, inode->block[12], 1, visitor, user_data,
                          readahead);

    if (inode->block[13])
        traverse_indirect(fs, inode, inode->block[13], 2, visitor, user_data,
                          readahead);

    if (inode->block[14])
        traverse_indirect(fs, inode, inode->block[14], 3, visitor, user_data,
                          readahead);
}

bool ext2_walk_dir(struct ext2_fs *fs, struct ext2_full_inode *dir,
                   dir_entry_callback cb, void *ctx) {
    return do_walk_dir(fs, dir, cb, ctx, false);
}

MAKE_NOP_CALLBACK;

bool ext2_find_first_available(struct ext2_fs *fs, struct ext2_full_inode *dir,
                               uint32_t *new_block) {
    return do_walk_dir(fs, dir, nop_callback, new_block, true);
}
