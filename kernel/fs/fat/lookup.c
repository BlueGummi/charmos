#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

struct fat_lookup_ctx {
    const char *fname;
    struct fat_dirent *found_dir; // if this remains NULL, we never found it
    // no need for 'found' field
};

static bool fat_lookup_cb(struct fat_dirent *d, void *c) {
    struct fat_lookup_ctx *ctx = c;
    if (memcmp(d->name, ctx->fname, 11) == 0) {
        ctx->found_dir = d;
        return true;
    }
    return false;
}

struct fat_dirent *fat_lookup(struct fat_fs *fs, uint32_t cluster,
                              const char *f) {
    char fmtname[11];
    fat_format_filename_83(f, fmtname);
    struct fat_lookup_ctx ctx = {.found_dir = NULL, .fname = fmtname};
    fat_walk_cluster(fs, cluster, fat_lookup_cb, &ctx);
    return ctx.found_dir;
}

bool fat_contains(struct fat_fs *fs, uint32_t cluster, const char *f) {
    struct fat_dirent *d = fat_lookup(fs, cluster, f);
    if (d) {
        kfree(d);
        return true;
    }
    return false;
}
