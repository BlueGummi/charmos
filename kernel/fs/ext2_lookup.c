#include <fs/ext2.h>
#include <string.h>
#include <vmalloc.h>

struct search_ctx {
    const char *target;
    struct ext2_inode *result;
};

static bool search_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry,
                            void *ctx_ptr) {
    struct search_ctx *ctx = (struct search_ctx *) ctx_ptr;

    if (entry->inode != 0 &&
        memcmp(entry->name, ctx->target, entry->name_len) == 0 &&
        ctx->target[entry->name_len] == '\0') {

        ctx->result = kmalloc(sizeof(struct ext2_inode));
        ext2_read_inode(fs, entry->inode, ctx->result);
        return true;
    }

    return false;
}

struct ext2_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                         const struct ext2_inode *dir_inode,
                                         const char *fname) {
    struct search_ctx ctx = {
        .target = fname,
        .result = NULL
    };

    walk_directory_entries(fs, dir_inode, search_callback, &ctx);

    return ctx.result;
}

