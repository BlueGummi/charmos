#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <string.h>

// TODO: With all VFS impls, make sure to check these are on the same disk and
// filesystem

struct vfs_node *ext2_vfs_finddir(struct vfs_node *node, const char *fname);
static enum vfs_node_type ext2_to_vfs_type(uint16_t mode);
static uint32_t ext2_to_vfs_flags(uint32_t ext2_flags);
static enum errno ext2_to_vfs_stat(struct ext2_full_inode *node,
                                   struct vfs_stat *out);

static struct vfs_ops ext2_vfs_ops = {
    .finddir = ext2_vfs_finddir,

};

//
//
// Utility functions and things - converting between ext2/vfs versions of stuff
//
//

static enum errno ext2_to_vfs_stat(struct ext2_full_inode *node,
                                   struct vfs_stat *out) {
    if (!out || !node)
        return ERR_INVAL;

    struct ext2_inode *inode = &node->node;

    out->inode = node->inode_num;
    out->type = ext2_to_vfs_type(inode->mode);
    out->nlink = inode->links_count;
    out->atime = inode->atime;
    out->mtime = inode->mtime;
    out->ctime = inode->ctime;
    return ERR_OK;
}

static uint32_t ext2_to_vfs_flags(uint32_t ext2_flags) {
    uint32_t vfs_flags = 0;

    if (ext2_flags & EXT2_APPEND_FL)
        vfs_flags |= VFS_NODE_APPENDONLY;

    if (ext2_flags & EXT2_IMMUTABLE_FL)
        vfs_flags |= VFS_NODE_IMMUTABLE;

    if (ext2_flags & EXT2_NOATIME_FL)
        vfs_flags |= VFS_NODE_NOATIME;

    if (ext2_flags & EXT2_SYNC_FL)
        vfs_flags |= VFS_NODE_SYNC;

    if (ext2_flags & EXT2_DIRSYNC_FL)
        vfs_flags |= VFS_NODE_DIRSYNC;

    return vfs_flags;
}

static enum vfs_node_type ext2_to_vfs_type(uint16_t type) {
    switch (type & EXT2_S_IFMT) {
    case EXT2_S_IFREG: return VFS_FILE;
    case EXT2_S_IFDIR: return VFS_DIR;
    case EXT2_S_IFLNK: return VFS_SYMLINK;
    case EXT2_S_IFCHR: return VFS_CHARDEV;
    case EXT2_S_IFBLK: return VFS_BLOCKDEV;
    case EXT2_S_IFIFO: return VFS_PIPE;
    case EXT2_S_IFSOCK: return VFS_SOCKET;
    default: return VFS_FILE; // fallback
    }
}

static uint16_t vfs_to_ext2_type(enum vfs_node_type type) {
    switch (type) {
    case VFS_FILE: return EXT2_S_IFREG;
    case VFS_DIR: return EXT2_S_IFDIR;
    case VFS_SYMLINK: return EXT2_S_IFLNK;
    case VFS_CHARDEV: return EXT2_S_IFCHR;
    case VFS_BLOCKDEV: return EXT2_S_IFBLK;
    case VFS_PIPE: return EXT2_S_IFIFO;
    case VFS_SOCKET: return EXT2_S_IFSOCK;
    default: return EXT2_S_IFREG; // fallback
    }
}

static struct vfs_node *make_vfs_node(struct ext2_fs *fs,
                                      struct ext2_full_inode *node,
                                      const char *fname) {
    if (!node || !fname || !fs)
        return NULL;

    struct vfs_node *ret = kzalloc(sizeof(struct vfs_node));

    if (*fname == '.')
        ret->flags |= VFS_NODE_HIDDEN;

    memcpy(ret->name, fname, strlen(fname));

    ret->flags = ext2_to_vfs_flags(node->node.flags);

    ret->fs_data = fs;
    ret->fs_node_data = node;
    ret->size = node->node.size;
    ret->type = ext2_to_vfs_type(node->node.mode);
    ret->ops = &ext2_vfs_ops;

    return ret;
}

//
//
//
// Actual implementations are below this
//
//
//

struct vfs_node *ext2_vfs_finddir(struct vfs_node *node, const char *fname) {
    struct ext2_full_inode *full_inode = node->fs_node_data;

    struct ext2_fs *fs = node->fs_data;

    struct ext2_full_inode *found =
        ext2_find_file_in_dir(fs, full_inode, fname, NULL);

    return make_vfs_node(node->fs_data, found, fname);
}

struct rename_ctx {
    const char *old_name;
    const char *new_name;
    bool success;
};

static bool dir_entry_rename_callback(struct ext2_fs *fs,
                                      struct ext2_dir_entry *entry,
                                      void *ctx_ptr, uint32_t b, uint32_t e_num,
                                      uint32_t c) {
    (void) c, (void) fs, (void) b, (void) e_num;
    struct rename_ctx *ctx = (struct rename_ctx *) ctx_ptr;

    if (entry->inode != 0 &&
        memcmp(entry->name, ctx->old_name, entry->name_len) == 0 &&
        ctx->old_name[entry->name_len] == '\0') {
        memcpy(entry->name, ctx->new_name, strlen(ctx->new_name));
        ctx->success = true;
        return true;
    }

    return false;
}

static enum errno dir_entry_rename(struct ext2_fs *fs,
                                   struct ext2_full_inode *node,
                                   const char *old, const char *new) {
    if (!ext2_dir_contains_file(fs, node, old))
        return ERR_NO_ENT;

    struct rename_ctx ctx = {
        .old_name = old, .new_name = new, .success = false};

    if (!ext2_walk_dir(fs, node, dir_entry_rename_callback, &ctx, false))
        return ERR_FS_INTERNAL;

    return ERR_OK;
}

enum errno ext2_vfs_rename(struct vfs_node *old_parent, const char *old_name,
                           struct vfs_node *new_parent, const char *new_name) {

    struct ext2_fs *fs = old_parent->fs_data;
    struct ext2_full_inode *old_node = old_parent->fs_node_data;
    struct ext2_full_inode *new_node = new_parent->fs_node_data;

    if (old_node == new_node) {
        return dir_entry_rename(fs, old_node, old_name, new_name);
    }

    uint8_t ftype = 0;

    struct ext2_full_inode *this_inode =
        ext2_find_file_in_dir(fs, old_node, old_name, &ftype);

    if (!this_inode)
        return ERR_NO_ENT;

    ext2_unlink_file(fs, old_node, old_name, false);

    return ext2_link_file(fs, new_node, this_inode, new_name, ftype);
}

enum errno ext2_vfs_stat(struct vfs_node *v, struct vfs_stat *out) {
    struct ext2_full_inode *node = v->fs_node_data;
    return ext2_to_vfs_stat(node, out);
}

enum errno ext2_vfs_link(struct vfs_node *parent, struct vfs_node *target,
                         const char *name) {
    if (target->type == VFS_DIR)
        return ERR_IS_DIR;

    // TODO: check if this is a directory
    struct ext2_full_inode *dir = parent->fs_node_data;
    struct ext2_full_inode *child = target->fs_node_data;
    struct ext2_fs *fs = parent->fs_data;

    return ext2_link_file(fs, dir, child, name, vfs_to_ext2_type(target->type));
}


