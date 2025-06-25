#include <console/printf.h>
#include <errno.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

struct link_ctx {
    const char *name;
    uint32_t inode;
    uint32_t dir_inode;
    uint8_t type;
    bool success;
};

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
    uint32_t needed_size = 8 + ((strlen(ctx->name) + 3) & ~3U);

    if ((entry->rec_len - actual_size) >= needed_size) {
        uint8_t *entry_base = (uint8_t *) entry;

        uint32_t original_rec_len = entry->rec_len;

        entry->rec_len = actual_size;

        struct ext2_dir_entry *new_entry =
            (struct ext2_dir_entry *) (entry_base + actual_size);

        new_entry->inode = ctx->inode;
        new_entry->name_len = strlen(ctx->name);
        new_entry->rec_len = original_rec_len - actual_size;
        new_entry->file_type = ctx->type;

        memcpy(new_entry->name, ctx->name, new_entry->name_len);
        new_entry->name[new_entry->name_len] = '\0';
        ctx->success = true;
        return true;
    }

    return false;
}

enum errno ext2_link_file(struct ext2_fs *fs, struct ext2_full_inode *dir_inode,
                          struct ext2_full_inode *inode, const char *name,
                          uint8_t type, bool increment_links) {
    struct link_ctx ctx = {
        .name = name,
        .inode = inode->inode_num,
        .success = false,
        .dir_inode = dir_inode->inode_num,
        .type = type,
    };

    if (ext2_dir_contains_file(fs, dir_inode, name))
        return ERR_NO_ENT;

    ext2_walk_dir(fs, dir_inode, link_callback, &ctx, false);

    /* did not need to allocate new block */
    if (ctx.success)
        goto done;

    uint32_t new_block = ext2_alloc_block(fs);
    if (new_block == 0)
        return ERR_NOSPC;

    uint8_t *block_data = kzalloc(fs->block_size);
    if (!block_data)
        return ERR_NO_MEM;

    struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *) block_data;

    new_entry->inode = inode->inode_num;
    new_entry->name_len = strlen(name);
    new_entry->rec_len = fs->block_size;
    new_entry->file_type = type;

    memcpy(new_entry->name, name, new_entry->name_len);

    if (ext2_block_ptr_write(fs, new_block, (uint32_t *) block_data)) {
        if (!ext2_walk_dir(fs, dir_inode, nop_callback, &new_block, true)) {
            ext2_free_block(fs, new_block);
            return ERR_FS_INTERNAL;
        }
    } else {
        return ERR_FS_INTERNAL;
    }

    /* allocated new block */
    kfree(block_data);

done:
    if (increment_links)
        dir_inode->node.links_count += 1;

    return ext2_write_inode(fs, dir_inode->inode_num, &dir_inode->node)
               ? ERR_OK
               : ERR_FS_INTERNAL;
}

enum errno ext2_create_file(struct ext2_fs *fs,
                            struct ext2_full_inode *parent_dir,
                            const char *name, uint16_t mode,
                            bool increment_links) {
    uint32_t new_inode_num = ext2_alloc_inode(fs);
    if (new_inode_num == 0)
        return ERR_NOSPC;

    struct ext2_inode new_inode = {0};
    ext2_init_inode(&new_inode, mode);

    if (!ext2_write_inode(fs, new_inode_num, &new_inode))
        return ERR_IO;

    struct ext2_full_inode temp_full_inode = {
        .node = new_inode,
        .inode_num = new_inode_num,
    };

    uint8_t file_type = ext2_extract_ftype(mode);

    enum errno err = ext2_link_file(fs, parent_dir, &temp_full_inode, name,
                                    file_type, increment_links);

    if (err != ERR_OK) {
        ext2_free_inode(fs, new_inode_num);
        return err;
    }

    return ERR_OK;
}
