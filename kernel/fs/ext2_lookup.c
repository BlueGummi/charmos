#include <fs/ext2.h>
#include <string.h>
#include <vmalloc.h>

struct search_ctx {
    const char *target;
    struct k_full_inode *result;
};

struct contains_ctx {
    const char *target;
    bool found;
};

static bool search_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                            void *ctx_ptr, uint32_t block_num) {
    struct search_ctx *ctx = (struct search_ctx *) ctx_ptr;

    if (entry->inode != 0 &&
        memcmp(entry->name, ctx->target, entry->name_len) == 0 &&
        ctx->target[entry->name_len] == '\0') {
        ctx->result->inode_num = block_num;
        ctx->result = kmalloc(sizeof(struct ext2_inode));
        ext2_read_inode(fs, entry->inode, &ctx->result->node);
        return true;
    }

    return false;
}

static bool contains_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                              void *ctx_ptr, uint32_t block_num) {
    struct contains_ctx *ctx = (struct contains_ctx *) ctx_ptr;
    (void) block_num;
    (void) fs;
    if (entry->inode != 0 &&
        memcmp(entry->name, ctx->target, entry->name_len) == 0 &&
        ctx->target[entry->name_len] == '\0') {
        ctx->found = true;
        return true;
    }

    ctx->found = false;
    return false;
}

struct k_full_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                           struct k_full_inode *dir_inode,
                                           const char *fname) {
    struct search_ctx ctx = {.target = fname, .result = NULL};

    ext2_walk_dir(fs, dir_inode, search_callback, &ctx, false);
    return ctx.result;
}

bool ext2_dir_contains_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                            const char *fname) {
    struct contains_ctx ctx = {.target = fname, .found = false};

    ext2_walk_dir(fs, dir_inode, contains_callback, &ctx, false);

    return ctx.found;
}
