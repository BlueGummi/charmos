#include <console/printf.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <string.h>

struct search_ctx {
    const char *target;
    struct ext2_full_inode *result;
    uint8_t type;
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

        ctx->result->inode_num = e_num;
        ctx->type = entry->file_type;
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

struct ext2_full_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                              struct ext2_full_inode *dir_inode,
                                              const char *fname,
                                              uint8_t *type_out) {
    struct ext2_full_inode out_node = {0};
    struct search_ctx ctx = {.target = fname, .result = &out_node, .type = 0};
    ext2_walk_dir(fs, dir_inode, search_callback, &ctx, false);

    if (type_out)
        *type_out = ctx.type;
    return ctx.result;
}

bool ext2_dir_contains_file(struct ext2_fs *fs,
                            struct ext2_full_inode *dir_inode,
                            const char *fname) {
    struct contains_ctx ctx = {.target = fname, .found = false};

    ext2_walk_dir(fs, dir_inode, contains_callback, &ctx, false);

    return ctx.found;
}
