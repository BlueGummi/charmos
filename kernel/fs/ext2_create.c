#include <alloc.h>
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

MAKE_NOP_CALLBACK;

static bool link_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                          void *ctx_ptr, uint32_t block_num, uint32_t e) {
    (void) fs; // dont complain compiler
    (void) block_num;
    (void) e;
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
        new_entry->file_type = EXT2_FT_REG_FILE;
        memcpy(new_entry->name, ctx->name, new_entry->name_len);
        new_entry->name[new_entry->name_len] = '\0';
        ctx->success = true;
        return true;
    }

    return false;
}

bool ext2_link_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                    struct k_full_inode *inode, char *name) {
    struct link_ctx ctx = {
        .name = name,
        .inode = inode->inode_num,
        .success = false,
        .dir_inode = dir_inode->inode_num,
    };

    if (ext2_dir_contains_file(fs, dir_inode, name))
        return false;

    ext2_walk_dir(fs, dir_inode, link_callback, &ctx, false);
    if (ctx.success) {
        dir_inode->node.links_count += 1;
        return true;
    }

    uint32_t new_block = ext2_alloc_block(fs);
    if (new_block == 0) {
        return false;
    }

    uint8_t *block_data = kzalloc(fs->block_size);

    struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *) block_data;
    new_entry->inode = inode->inode_num;
    new_entry->name_len = strlen(name);
    new_entry->rec_len = fs->block_size;
    new_entry->file_type = EXT2_FT_REG_FILE;
    memcpy(new_entry->name, name, new_entry->name_len);

    if (block_ptr_write(fs, new_block, (uint32_t *) block_data)) {
        if (!ext2_walk_dir(fs, dir_inode, nop_callback, &new_block, true)) {
            ext2_free_block(fs, new_block);
            return false;
        }
    } else {
        return false;
    }

    kfree(block_data);
    dir_inode->node.links_count += 1;
    return ext2_write_inode(fs, dir_inode->inode_num, &dir_inode->node);
}

bool ext2_create_inode(struct ext2_fs *fs, uint32_t inode_num, uint16_t mode,
                       uint16_t uid, uint16_t gid, uint32_t size,
                       uint32_t flags) {
    if (!fs)
        return false;

    struct ext2_inode inode = {0};

    inode.mode = mode;
    inode.uid = uid;
    inode.gid = gid;
    inode.size = size;
    inode.atime = inode.ctime = inode.mtime = 0;
    inode.dtime = 0;
    inode.links_count = 1;
    inode.flags = flags;
    inode.blocks = 0;
    inode.osd1 = 0;

    for (int i = 0; i < EXT2_NBLOCKS; i++)
        inode.block[i] = 0;

    return ext2_write_inode(fs, inode_num, &inode);
}
