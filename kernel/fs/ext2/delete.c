#include <block/bcache.h>
#include <errno.h>
#include <fs/ext2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

struct unlink_ctx {
    const char *name;
    bool found;
    uint32_t inode_num;
    uint32_t block_num;
    uint32_t entry_offset;
    uint32_t prev_offset;
};

void free_block_visitor(struct ext2_fs *fs, struct ext2_inode *inode,
                        uint32_t depth, uint32_t *block_ptr, void *user_data) {
    (void) inode, (void) user_data, (void) depth;
    if (*block_ptr) {
        ext2_free_block(fs, *block_ptr);
        *block_ptr = 0;
    }
}

bool unlink_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                     void *arg, uint32_t block_num, uint32_t e,
                     uint32_t entry_offset) {
    (void) e;
    (void) fs;
    struct unlink_ctx *ctx = (struct unlink_ctx *) arg;

    if (ctx->found)
        return false;

    if (entry->name_len == strlen(ctx->name) &&
        strncmp(entry->name, ctx->name, entry->name_len) == 0) {
        ctx->found = true;
        ctx->inode_num = entry->inode;
        ctx->block_num = block_num;
        ctx->entry_offset = entry_offset;
        entry->inode = 0;
        entry->name_len = 0;
        memset(entry->name, 0, EXT2_NAME_LEN);
        return true;
    }

    ctx->prev_offset = entry_offset;
    return false;
}

static void unlink_adjust_neighbors(struct ext2_fs *fs, uint8_t *block,
                                    uint32_t offset, uint32_t prev_offset) {
    struct ext2_dir_entry *entry = (struct ext2_dir_entry *) (block + offset);

    if (offset == 0) {
        struct ext2_dir_entry *next =
            (struct ext2_dir_entry *) ((uint8_t *) entry + entry->rec_len);
        if ((uint8_t *) next < block + fs->block_size && next->inode != 0) {
            next->rec_len += entry->rec_len;
        }
    } else {
        struct ext2_dir_entry *prev =
            (struct ext2_dir_entry *) (block + prev_offset);
        prev->rec_len += entry->rec_len;
    }
}

static inline void unlink_target_update(struct ext2_inode *target_inode) {
    target_inode->dtime = time_get_unix();
    target_inode->links_count--;
}

static inline void unlink_free_blocks(struct ext2_fs *fs,
                                      struct ext2_inode *target_inode,
                                      uint32_t inode_num) {
    ext2_traverse_inode_blocks(fs, target_inode, free_block_visitor, NULL,
                               false);
    ext2_free_inode(fs, inode_num);
}

enum errno ext2_unlink_file(struct ext2_fs *fs,
                            struct ext2_full_inode *dir_inode, const char *name,
                            bool free_blocks, bool decrement_links) {
    if (!ext2_dir_contains_file(fs, dir_inode, name))
        return ERR_NO_ENT;

    struct unlink_ctx ctx = {name, false, 0, 0, 0, 0};
    if (!ext2_walk_dir(fs, dir_inode, unlink_callback, &ctx))
        return ERR_FS_INTERNAL;

    struct bcache_entry *ent;
    uint8_t *block = ext2_block_read(fs, ctx.block_num, &ent);
    if (!block)
        return ERR_IO;

    unlink_adjust_neighbors(fs, block, ctx.entry_offset, ctx.prev_offset);
    bcache_ent_release(ent);

    if (!ext2_block_write(fs, ent, EXT2_PRIO_DIRENT))
        return ERR_IO;

    struct ext2_inode *target_inode = NULL;
    target_inode = ext2_inode_read(fs, ctx.inode_num, &ent);

    if (!ent || !target_inode)
        return ERR_IO;

    if (target_inode->links_count == 0) {
        bcache_ent_release(ent);
        return ERR_FS_NO_INODE;
    }

    unlink_target_update(target_inode);
    bcache_ent_release(ent);

    /* This will re-acquire our locks */
    if (target_inode->links_count == 0 && free_blocks)
        unlink_free_blocks(fs, target_inode, ctx.inode_num);

    if (!ext2_inode_write(fs, ctx.inode_num, target_inode))
        return ERR_IO;

    if (decrement_links)
        dir_inode->node.links_count--;

    if (!ext2_inode_write(fs, dir_inode->inode_num, &dir_inode->node))
        return ERR_IO;

    return ERR_OK;
}
