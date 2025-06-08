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

static void ext2_dump_dir(struct ext2_dir_entry *dir) {
    k_printf("File type is 0x%hhx\n", dir->file_type);
    k_printf("Inode is %u\n", dir->inode);
    k_printf("Name len is %u\n", dir->name_len);
    k_printf("Rec len is %u\n", dir->rec_len);
    k_printf("Name: ");
    for (int i = 0; i < dir->name_len; i++) {
        k_printf("%c", dir->name[i]);
    }
    k_printf("\n");
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
        return true;
    }

    ctx->prev_offset = entry_offset;
    return false;
}

enum errno ext2_unlink_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                            const char *name) {

    if (!ext2_dir_contains_file(fs, dir_inode, name))
        return ERR_NO_ENT;

    struct unlink_ctx ctx = {name, false, 0, 0, 0, 0};

    if (!ext2_walk_dir(fs, dir_inode, unlink_callback, &ctx, false))
        return ERR_FS_INTERNAL;

    uint8_t *block = kmalloc(fs->block_size);
    if (!ext2_block_ptr_read(fs, ctx.block_num, block)) {
        kfree(block);
        return ERR_FS_INTERNAL;
    }

    struct ext2_dir_entry *entry =
        (struct ext2_dir_entry *) (block + ctx.entry_offset);
    entry->inode = 0;

    if (ctx.entry_offset == 0) {
        struct ext2_dir_entry *next =
            (struct ext2_dir_entry *) ((uint8_t *) entry + entry->rec_len);
        if ((uint8_t *) next < block + fs->block_size && next->inode != 0) {
            next->rec_len += entry->rec_len;
        }
    } else {
        struct ext2_dir_entry *prev =
            (struct ext2_dir_entry *) (block + ctx.prev_offset);
        prev->rec_len += entry->rec_len;
    }

    if (!ext2_block_ptr_write(fs, ctx.block_num, block)) {
        kfree(block);
        return ERR_FS_INTERNAL;
    }
    kfree(block);

    struct k_full_inode target_inode;
    if (!ext2_read_inode(fs, ctx.inode_num, &target_inode.node))
        return ERR_FS_INTERNAL;

    target_inode.inode_num = ctx.inode_num;
    if (target_inode.node.links_count == 0) {
        kfree(block);
        return ERR_FS_NO_INODE;
    }

    target_inode.node.dtime = time_get_unix();

    if (--target_inode.node.links_count == 0) {
        ext2_traverse_inode_blocks(fs, &target_inode.node, free_block_visitor,
                                   NULL);
        ext2_free_inode(fs, ctx.inode_num);
    }

    if (!ext2_write_inode(fs, ctx.inode_num, &target_inode.node))
        return ERR_FS_INTERNAL;

    if (!ext2_write_inode(fs, dir_inode->inode_num, &dir_inode->node))
        return ERR_FS_INTERNAL;

    return ERR_OK;
}
