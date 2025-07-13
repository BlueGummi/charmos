#include <fs/ext2.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "errno.h"

struct search_ctx {
    const char *target;
    struct ext2_full_inode *result;
    uint8_t type;
    bool found;
};

struct contains_ctx {
    const char *target;
    bool found;
};

struct readdir_ctx {
    uint32_t entry_offset;
    struct ext2_dir_entry *out;
    bool found;
    uint32_t count;
};

/* TODO: Figure out how to return errors here in the case of a failed read */
static bool search_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                            void *ctx_ptr, uint32_t b, uint32_t e_num,
                            uint32_t offset) {
    (void) b;
    struct search_ctx *ctx = (struct search_ctx *) ctx_ptr;

    if (!ext2_dirent_valid(entry))
        return false;

    if (offset + entry->rec_len > fs->block_size)
        return false;

    if (memcmp(entry->name, ctx->target, entry->name_len) == 0 &&
        ctx->target[entry->name_len] == '\0') {
        ctx->found = true;
        ctx->result->inode_num = e_num;
        ctx->type = entry->file_type;

        struct ext2_inode *out = &ctx->result->node;
        uint32_t ino = entry->inode;
        struct bcache_entry *ent;

        /* read `acquire()'s the node, we have to unlock but not dec refcount */

        memcpy(out, ext2_inode_read(fs, ino, &ent), sizeof(struct ext2_inode));
        bcache_ent_unlock(ent);
        ctx->result->ent = ent;

        return true;
    }

    return false;
}

static bool contains_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                              void *ctx_ptr, uint32_t b, uint32_t e,
                              uint32_t offset) {
    struct contains_ctx *ctx = (struct contains_ctx *) ctx_ptr;
    (void) b;
    (void) e;

    if (!ext2_dirent_valid(entry))
        return false;

    if (offset + entry->rec_len > fs->block_size)
        return false;

    if (memcmp(entry->name, ctx->target, entry->name_len) == 0 &&
        ctx->target[entry->name_len] == '\0') {
        ctx->found = true;
        return true;
    }

    ctx->found = false;
    return false;
}

static bool readdir_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                             void *ctx_ptr, uint32_t block, uint32_t entry_num,
                             uint32_t entry_offset) {
    (void) fs, (void) block, (void) entry_num, (void) entry_offset;

    struct readdir_ctx *ctx = ctx_ptr;

    if (!ext2_dirent_valid(entry))
        return false;

    if (ctx->count == ctx->entry_offset) {
        memcpy(ctx->out, entry, sizeof(struct ext2_dir_entry));
        ctx->found = true;
        return true;
    }

    ctx->count++;
    return false;
}

struct ext2_full_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                              struct ext2_full_inode *dir_inode,
                                              const char *fname,
                                              uint8_t *type_out) {
    struct ext2_full_inode *out_node = kzalloc(sizeof(struct ext2_full_inode));
    struct search_ctx ctx = {.target = fname, .result = out_node, .type = 0};
    ext2_walk_dir(fs, dir_inode, search_callback, &ctx);

    if (type_out)
        *type_out = ctx.type;

    if (ctx.found) {
        return ctx.result;
    } else {
        kfree(out_node);
        return NULL;
    }
}

bool ext2_dir_contains_file(struct ext2_fs *fs,
                            struct ext2_full_inode *dir_inode,
                            const char *fname) {
    struct contains_ctx ctx = {.target = fname, .found = false};

    ext2_walk_dir(fs, dir_inode, contains_callback, &ctx);

    return ctx.found;
}

enum errno ext2_readdir(struct ext2_fs *fs, struct ext2_full_inode *dir_inode,
                        struct ext2_dir_entry *out, uint32_t entry_offset) {
    if (!fs || !dir_inode || !out)
        return ERR_INVAL;

    struct readdir_ctx ctx = {
        .out = out, .entry_offset = entry_offset, .count = 0};

    ext2_walk_dir(fs, dir_inode, readdir_callback, &ctx);

    return ctx.found ? ERR_OK : ERR_NO_ENT;
}
