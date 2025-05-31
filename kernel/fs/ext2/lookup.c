#include <mem/alloc.h>
#include <fs/ext2.h>
#include <console/printf.h>
#include <string.h>

struct search_ctx {
    const char *target;
    struct k_full_inode *result;
};

struct contains_ctx {
    const char *target;
    bool found;
};

static bool search_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                            void *ctx_ptr, uint32_t b, uint32_t e_num,
                            uint32_t c) {
    (void) c;
    (void) b;
    struct search_ctx *ctx = (struct search_ctx *) ctx_ptr;

    if (entry->inode != 0 &&
        memcmp(entry->name, ctx->target, entry->name_len) == 0 &&
        ctx->target[entry->name_len] == '\0') {
        ctx->result = kmalloc(sizeof(struct ext2_inode));
        ctx->result->inode_num = e_num;
        ext2_read_inode(fs, entry->inode, &ctx->result->node);
        return true;
    }

    return false;
}

static bool contains_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                              void *ctx_ptr, uint32_t b, uint32_t e,
                              uint32_t c) {
    struct contains_ctx *ctx = (struct contains_ctx *) ctx_ptr;
    (void) b;
    (void) e;
    (void) fs;
    (void) c;
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
    struct k_full_inode out_node = {0};
    struct search_ctx ctx = {.target = fname, .result = &out_node};
    ext2_walk_dir(fs, dir_inode, search_callback, &ctx, false);
    return ctx.result;
}

bool ext2_dir_contains_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                            const char *fname) {
    struct contains_ctx ctx = {.target = fname, .found = false};

    ext2_walk_dir(fs, dir_inode, contains_callback, &ctx, false);

    return ctx.found;
}
