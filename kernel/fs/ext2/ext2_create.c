#include <alloc.h>
#include <errno.h>
#include <fs/ext2.h>
#include <printf.h>
#include <stdint.h>
#include <string.h>

struct link_ctx {
    char *name;
    uint32_t inode;
    uint32_t dir_inode;
    bool success;
};

static bool make_symlink = false;

MAKE_NOP_CALLBACK;

static bool link_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                          void *ctx_ptr, uint32_t block_num, uint32_t e,
                          uint32_t o) {
    (void) fs; // dont complain compiler
    (void) block_num;
    (void) e;
    (void) o;
    struct link_ctx *ctx = (struct link_ctx *) ctx_ptr;

    uint32_t actual_size = 8 + ((entry->name_len + 3) & ~3); // alignment
    uint32_t needed_size = 8 + ((strlen(ctx->name) + 3) & ~3);

    if ((entry->rec_len - actual_size) >= needed_size) {
        uint8_t *entry_base = (uint8_t *) entry;

        uint32_t original_rec_len = entry->rec_len;

        entry->rec_len = actual_size;

        struct ext2_dir_entry *new_entry =
            (struct ext2_dir_entry *) (entry_base + actual_size);
        new_entry->inode = ctx->inode;
        new_entry->name_len = strlen(ctx->name);
        new_entry->rec_len = original_rec_len - actual_size;

        if (make_symlink) {
            new_entry->file_type = EXT2_FT_SYMLINK;
        } else {
            new_entry->file_type = EXT2_FT_REG_FILE;
        }

        memcpy(new_entry->name, ctx->name, new_entry->name_len);
        new_entry->name[new_entry->name_len] = '\0';
        ctx->success = true;
        return true;
    }

    return false;
}

enum errno ext2_link_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                          struct k_full_inode *inode, char *name) {
    struct link_ctx ctx = {
        .name = name,
        .inode = inode->inode_num,
        .success = false,
        .dir_inode = dir_inode->inode_num,
    };

    if (ext2_dir_contains_file(fs, dir_inode, name))
        return ERR_NO_ENT;

    ext2_walk_dir(fs, dir_inode, link_callback, &ctx, false);
    if (ctx.success) {
        dir_inode->node.links_count += 1;
        return ERR_OK;
    }

    uint32_t new_block = ext2_alloc_block(fs);
    if (new_block == 0) {
        return ERR_FS_NO_INODE;
    }

    uint8_t *block_data = kzalloc(fs->block_size);

    struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *) block_data;
    new_entry->inode = inode->inode_num;
    new_entry->name_len = strlen(name);
    new_entry->rec_len = fs->block_size;

    if (make_symlink) {
        new_entry->file_type = EXT2_FT_SYMLINK;
    } else {
        new_entry->file_type = EXT2_FT_REG_FILE;
    }

    memcpy(new_entry->name, name, new_entry->name_len);

    if (block_ptr_write(fs, new_block, (uint32_t *) block_data)) {
        if (!ext2_walk_dir(fs, dir_inode, nop_callback, &new_block, true)) {
            ext2_free_block(fs, new_block);
            return ERR_FS_INTERNAL;
        }
    } else {
        return ERR_FS_INTERNAL;
    }

    kfree(block_data);
    dir_inode->node.links_count += 1;
    return ext2_write_inode(fs, dir_inode->inode_num, &dir_inode->node)
               ? ERR_OK
               : ERR_FS_INTERNAL;
}

enum errno ext2_symlink_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                             const char *name, char *target) {
    uint32_t inode_num = ext2_alloc_inode(fs);
    if (inode_num == 0)
        return ERR_FS_NO_INODE;

    struct ext2_inode new_inode = {0};
    new_inode.ctime = get_unix_time();
    new_inode.mode = EXT2_S_IFLNK | 0777;
    new_inode.links_count = 1;
    new_inode.size = strlen(target);
    new_inode.blocks = 0;

    if (strlen(target) <= sizeof(new_inode.block)) {
        memcpy(new_inode.block, target, strlen(target));
        new_inode.block[strlen(target)] = '\0';
    } else {
        uint32_t block = ext2_alloc_block(fs);
        if (block == 0)
            return ERR_FS_NO_INODE;

        block_ptr_write(fs, block, target);
        new_inode.block[0] = block;
        new_inode.blocks = fs->block_size / 512;
    }

    if (!ext2_write_inode(fs, inode_num, &new_inode))
        return ERR_FS_INTERNAL;

    struct k_full_inode wrapped_inode = {
        .inode_num = inode_num,
        .node = new_inode,
    };

    make_symlink = true;
    enum errno ret =
        ext2_link_file(fs, dir_inode, &wrapped_inode, (char *) name);
    make_symlink = false;
    return ret;
}
