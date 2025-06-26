#include <console/printf.h>
#include <errno.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
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

static void unlink_target_update(struct ext2_full_inode *target_inode,
                                 uint32_t inode_num) {
    target_inode->inode_num = inode_num;
    target_inode->node.dtime = time_get_unix();
    target_inode->node.links_count--;
}

static void unlink_free_blocks(struct ext2_fs *fs,
                               struct ext2_full_inode *target_inode,
                               uint32_t inode_num) {
    ext2_traverse_inode_blocks(fs, &target_inode->node, free_block_visitor,
                               NULL);
    ext2_free_inode(fs, inode_num);
}

#define UNLINK_RETURN_ERR(err, type)                                           \
    do {                                                                       \
        err = type;                                                            \
        goto cleanup;                                                          \
    } while (0)

#define TRY(x)                                                                 \
    do {                                                                       \
        if (!(x))                                                              \
            UNLINK_RETURN_ERR(err, ERR_FS_INTERNAL);                           \
    } while (0)

enum errno ext2_unlink_file(struct ext2_fs *fs,
                            struct ext2_full_inode *dir_inode, const char *name,
                            bool free_blocks, bool decrement_links) {
    if (!ext2_dir_contains_file(fs, dir_inode, name))
        return ERR_NO_ENT;

    struct unlink_ctx ctx = {name, false, 0, 0, 0, 0};
    if (!ext2_walk_dir(fs, dir_inode, unlink_callback, &ctx, false))
        return ERR_FS_INTERNAL;

    uint8_t *block = kzalloc(fs->block_size);
    if (!block)
        return ERR_NO_MEM;

    enum errno err = ERR_OK;
    struct ext2_full_inode target_inode;

    TRY(ext2_block_read(fs, ctx.block_num, block));

    unlink_adjust_neighbors(fs, block, ctx.entry_offset, ctx.prev_offset);

    TRY(ext2_block_write(fs, ctx.block_num, block));
    TRY(ext2_inode_read(fs, ctx.inode_num, &target_inode.node));

    if (target_inode.node.links_count == 0)
        UNLINK_RETURN_ERR(err, ERR_FS_NO_INODE);

    unlink_target_update(&target_inode, ctx.inode_num);

    if (target_inode.node.links_count == 0 && free_blocks)
        unlink_free_blocks(fs, &target_inode, ctx.inode_num);

    TRY(ext2_inode_write(fs, ctx.inode_num, &target_inode.node));

    if (decrement_links)
        dir_inode->node.links_count--;

    TRY(ext2_inode_write(fs, dir_inode->inode_num, &dir_inode->node));

cleanup:
    if (block)
        kfree(block);
    return err;
}
