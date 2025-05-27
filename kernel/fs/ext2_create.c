#include <fs/ext2.h>
#include <printf.h>
#include <string.h>
#include <vmalloc.h>

struct link_ctx {
    char *name;
    uint32_t inode;
    uint32_t dir_inode;
    bool success;
};

static bool link_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                          void *ctx_ptr) {
    (void) fs; // dont complain compiler
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

        ctx->success = true;
        k_printf("MADE IT!\n");
        return true;
    }

    return false;
}

bool ext2_link_file(struct ext2_fs *fs, struct ext2_inode *dir_inode,
                    uint32_t dir_inode_num, uint32_t file_inode, char *name) {
    struct link_ctx ctx = {
        .name = name,
        .inode = file_inode,
        .success = false,
        .dir_inode = dir_inode_num,
    };

    walk_directory_entries(fs, dir_inode, link_callback, &ctx);
    if (ctx.success)
        return true;

    uint32_t new_block = ext2_alloc_block(fs);
    if (new_block == 0)
        return false;

    bool added = false;
    for (int i = 0; i < 12; i++) {
        if (dir_inode->block[i] == 0) {
            dir_inode->block[i] = new_block;
            dir_inode->blocks += fs->block_size / fs->drive->sector_size;
            dir_inode->size += fs->block_size;
            added = true;
            break;
        }
    }

    if (!added) {
        ext2_free_block(fs, new_block);
        return false;
    }

    uint8_t *block_data = kmalloc(fs->block_size);
    memset(block_data, 0, fs->block_size);

    struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *) block_data;
    new_entry->inode = file_inode;
    new_entry->name_len = strlen(name);
    new_entry->rec_len = fs->block_size;
    new_entry->file_type = EXT2_FT_REG_FILE;
    memcpy(new_entry->name, name, new_entry->name_len);

    uint32_t lba = (new_block * fs->block_size) / fs->drive->sector_size;
    if (!block_write(fs->drive, lba, block_data, fs->block_size / fs->drive->sector_size)) {
        kfree(block_data, fs->block_size);
        for (int i = 0; i < 12; i++) {
            if (dir_inode->block[i] == new_block) {
                dir_inode->block[i] = 0;
                break;
            }
        }
        ext2_free_block(fs, new_block);
        return false;
    }

    kfree(block_data, fs->block_size);
    k_printf("gotta make some space...\n");
    return ext2_write_inode(fs, dir_inode_num, dir_inode);
}
