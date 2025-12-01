#include <fs/fat.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct fat_lookup_ctx {
    const char *fname;
    struct fat_dirent *found_dir; // if this remains NULL, we never found it
    // no need for 'found' field
    uint32_t index;
};

static bool fat_lookup_cb(struct fat_dirent *d, uint32_t indx, void *c) {
    struct fat_lookup_ctx *ctx = c;
    if (memcmp(d->name, ctx->fname, 11) == 0) {
        ctx->found_dir = d;
        ctx->index = indx;
        return true;
    }
    return false;
}

struct fat_dirent *fat_lookup(struct fat_fs *fs, uint32_t cluster,
                              const char *f, uint32_t *out_index) {
    char fmtname[11];
    fat_format_filename_83(f, fmtname);
    struct fat_lookup_ctx ctx = {
        .found_dir = NULL, .fname = fmtname, .index = 0};

    fat_walk_cluster(fs, cluster, fat_lookup_cb, &ctx);
    if (out_index && ctx.found_dir) {
        *out_index = ctx.index;
    }
    return ctx.found_dir;
}

bool fat_contains(struct fat_fs *fs, uint32_t cluster, const char *f) {
    struct fat_dirent *d = fat_lookup(fs, cluster, f, NULL);
    if (d) {
        kfree(d, FREE_PARAMS_DEFAULT);
        return true;
    }
    return false;
}
