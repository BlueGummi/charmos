#include <console/printf.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>
#include <time/time.h>

// TODO: With all VFS impls, make sure to check these are on the same disk and
// filesystem

// TODO: open/close atomicity or whatever

// TODO: check if files already exist, update times, check if file types are
// correct

enum errno ext2_vfs_stat(struct vfs_node *v, struct vfs_stat *out);
enum errno ext2_vfs_rename(struct vfs_node *old_parent, const char *old_name,
                           struct vfs_node *new_parent, const char *new_name);
enum errno ext2_vfs_chmod(struct vfs_node *n, uint16_t mode);

enum errno ext2_vfs_chown(struct vfs_node *n, uint32_t uid, uint32_t gid);
enum errno ext2_vfs_utime(struct vfs_node *n, uint64_t atime, uint64_t mtime);

enum errno ext2_vfs_read(struct vfs_node *n, void *buf, uint64_t size,
                         uint64_t offset);

enum errno ext2_vfs_write(struct vfs_node *n, const void *buf, uint64_t size,
                          uint64_t offset);

enum errno ext2_vfs_truncate(struct vfs_node *n, uint64_t length);

enum errno ext2_vfs_link(struct vfs_node *parent, struct vfs_node *target,
                         const char *name);

enum errno ext2_vfs_create(struct vfs_node *n, const char *name, uint16_t mode);

enum errno ext2_vfs_unlink(struct vfs_node *n, const char *name);

enum errno ext2_vfs_symlink(struct vfs_node *parent, const char *target,
                            const char *link_name);

struct vfs_node *ext2_vfs_finddir(struct vfs_node *node, const char *fname);
enum errno ext2_vfs_mkdir(struct vfs_node *n, const char *name, uint16_t mode);
enum errno ext2_vfs_rmdir(struct vfs_node *n, const char *name);

enum errno vfs_dummy_open(struct vfs_node *a, uint32_t b) {
    (void) a, (void) b;
    return ERR_OK;
}

enum errno vfs_dummy_close(struct vfs_node *a) {
    (void) a;
    return ERR_OK;
}

static struct vfs_ops ext2_vfs_ops = {
    .open = vfs_dummy_open,
    .close = vfs_dummy_close,
    .stat = ext2_vfs_stat,
    .rename = ext2_vfs_rename,
    .chmod = ext2_vfs_chmod,
    .chown = ext2_vfs_chown,
    .utime = ext2_vfs_utime,
    .read = ext2_vfs_read,
    .write = ext2_vfs_write,
    .truncate = ext2_vfs_truncate,
    .link = ext2_vfs_link,
    .create = ext2_vfs_create,
    .unlink = ext2_vfs_unlink,
    .symlink = ext2_vfs_symlink,
    .finddir = ext2_vfs_finddir,
    .mkdir = ext2_vfs_mkdir,
    .rmdir = ext2_vfs_rmdir,
    .mount = vfs_mount,
    .unmount = vfs_unmount,
};

//
//
// Utility functions and things - converting between ext2/vfs versions of stuff
//
//

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

static uint32_t vfs_to_ext2_flags(uint32_t vfs_flags) {
    uint32_t ext2_flags = 0;

    if (vfs_flags & VFS_NODE_APPENDONLY)
        ext2_flags |= EXT2_APPEND_FL;

    if (vfs_flags & VFS_NODE_IMMUTABLE)
        ext2_flags |= EXT2_IMMUTABLE_FL;

    if (vfs_flags & VFS_NODE_NOATIME)
        ext2_flags |= EXT2_NOATIME_FL;

    if (vfs_flags & VFS_NODE_SYNC)
        ext2_flags |= EXT2_SYNC_FL;

    if (vfs_flags & VFS_NODE_DIRSYNC)
        ext2_flags |= EXT2_DIRSYNC_FL;

    return ext2_flags;
}

static uint16_t ext2_to_vfs_mode(uint16_t ext2_mode) {
    uint16_t vfs_mode = 0;

    // File type
    switch (ext2_mode & EXT2_S_IFMT) {
    case EXT2_S_IFREG: vfs_mode |= VFS_MODE_FILE; break;
    case EXT2_S_IFDIR: vfs_mode |= VFS_MODE_DIR; break;
    case EXT2_S_IFLNK: vfs_mode |= VFS_MODE_SYMLINK; break;
    case EXT2_S_IFCHR: vfs_mode |= VFS_MODE_CHARDEV; break;
    case EXT2_S_IFBLK: vfs_mode |= VFS_MODE_BLOCKDEV; break;
    case EXT2_S_IFIFO: vfs_mode |= VFS_MODE_PIPE; break;
    case EXT2_S_IFSOCK: vfs_mode |= VFS_MODE_SOCKET; break;
    default: vfs_mode |= VFS_MODE_FILE; break;
    }

    // Owner permissions
    if (ext2_mode & EXT2_S_IRUSR)
        vfs_mode |= VFS_MODE_READ;
    if (ext2_mode & EXT2_S_IWUSR)
        vfs_mode |= VFS_MODE_WRITE;
    if (ext2_mode & EXT2_S_IXUSR)
        vfs_mode |= VFS_MODE_EXEC;

    // Group permissions
    if (ext2_mode & EXT2_S_IRGRP)
        vfs_mode |= VFS_MODE_G_READ;
    if (ext2_mode & EXT2_S_IWGRP)
        vfs_mode |= VFS_MODE_G_WRITE;
    if (ext2_mode & EXT2_S_IXGRP)
        vfs_mode |= VFS_MODE_G_EXEC;

    // Other permissions
    if (ext2_mode & EXT2_S_IROTH)
        vfs_mode |= VFS_MODE_O_READ;
    if (ext2_mode & EXT2_S_IWOTH)
        vfs_mode |= VFS_MODE_O_WRITE;
    if (ext2_mode & EXT2_S_IXOTH)
        vfs_mode |= VFS_MODE_O_EXEC;

    return vfs_mode;
}

static uint16_t vfs_to_ext2_mode(uint16_t vfs_mode) {
    uint16_t ext2_mode = 0;

    // File type
    if (vfs_mode & VFS_MODE_DIR)
        ext2_mode |= EXT2_S_IFDIR;
    else if (vfs_mode & VFS_MODE_SYMLINK)
        ext2_mode |= EXT2_S_IFLNK;
    else if (vfs_mode & VFS_MODE_CHARDEV)
        ext2_mode |= EXT2_S_IFCHR;
    else if (vfs_mode & VFS_MODE_BLOCKDEV)
        ext2_mode |= EXT2_S_IFBLK;
    else if (vfs_mode & VFS_MODE_PIPE)
        ext2_mode |= EXT2_S_IFIFO;
    else if (vfs_mode & VFS_MODE_SOCKET)
        ext2_mode |= EXT2_S_IFSOCK;
    else
        ext2_mode |= EXT2_S_IFREG;

    // Owner permissions
    if (vfs_mode & VFS_MODE_READ)
        ext2_mode |= EXT2_S_IRUSR;
    if (vfs_mode & VFS_MODE_WRITE)
        ext2_mode |= EXT2_S_IWUSR;
    if (vfs_mode & VFS_MODE_EXEC)
        ext2_mode |= EXT2_S_IXUSR;

    // Group permissions
    if (vfs_mode & VFS_MODE_G_READ)
        ext2_mode |= EXT2_S_IRGRP;
    if (vfs_mode & VFS_MODE_G_WRITE)
        ext2_mode |= EXT2_S_IWGRP;
    if (vfs_mode & VFS_MODE_G_EXEC)
        ext2_mode |= EXT2_S_IXGRP;

    // Other permissions
    if (vfs_mode & VFS_MODE_O_READ)
        ext2_mode |= EXT2_S_IROTH;
    if (vfs_mode & VFS_MODE_O_WRITE)
        ext2_mode |= EXT2_S_IWOTH;
    if (vfs_mode & VFS_MODE_O_EXEC)
        ext2_mode |= EXT2_S_IXOTH;

    return ext2_mode;
}

static enum errno ext2_to_vfs_stat(struct ext2_full_inode *node,
                                   struct vfs_stat *out) {
    if (!out || !node)
        return ERR_INVAL;

    struct ext2_inode *inode = &node->node;

    out->inode = node->inode_num;
    out->mode = ext2_to_vfs_mode(inode->mode);
    out->nlink = inode->links_count;
    out->atime = inode->atime;
    out->mtime = inode->mtime;
    out->ctime = inode->ctime;
    return ERR_OK;
}

static struct vfs_node *make_vfs_node(struct ext2_fs *fs,
                                      struct ext2_full_inode *node,
                                      const char *fname) {
    if (!node || !fname || !fs)
        return NULL;

    struct vfs_node *ret = kzalloc(sizeof(struct vfs_node));
    if (!ret)
        return NULL;

    if (*fname == '.')
        ret->flags |= VFS_NODE_HIDDEN;

    memcpy(ret->name, fname, strlen(fname));

    ret->flags = ext2_to_vfs_flags(node->node.flags);

    ret->unique_id = node->inode_num;
    ret->fs_data = fs;
    ret->fs_node_data = node;
    ret->size = node->node.size;
    ret->mode = ext2_to_vfs_mode(node->node.mode);
    ret->ops = &ext2_vfs_ops;
    ret->fs_type = FS_EXT2;

    return ret;
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

//
//
//
// Actual implementations are below this
//
//
//

enum errno ext2_mount(struct generic_partition *p, struct ext2_fs *fs,
                      struct ext2_sblock *sblock, struct vfs_node *out_node) {
    if (!fs || !sblock)
        return ERR_INVAL;

    sblock->mtime = time_get_unix();
    sblock->wtime = time_get_unix();
    fs->drive = p->disk;
    fs->partition = p;
    fs->sblock = sblock;
    fs->inodes_count = sblock->inodes_count;
    fs->inodes_per_group = sblock->inodes_per_group;
    fs->inode_size = sblock->inode_size;
    fs->block_size = 1024U << sblock->log_block_size;

    fs->sectors_per_block = fs->block_size / p->disk->sector_size;

    fs->num_groups =
        (fs->inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;

    uint32_t gdt_block = (fs->block_size == 1024) ? 2 : 1;

    uint32_t gdt_bytes = fs->num_groups * sizeof(struct ext2_group_desc);
    uint32_t gdt_blocks = (gdt_bytes + fs->block_size - 1) / fs->block_size;

    fs->group_desc = kmalloc(gdt_blocks * fs->block_size);
    if (!fs->group_desc)
        return ERR_NO_MEM;

    if (!ext2_block_read(fs->partition, gdt_block * fs->sectors_per_block,
                         (uint8_t *) fs->group_desc,
                         gdt_blocks * fs->sectors_per_block)) {
        kfree(fs->group_desc);
        return ERR_IO;
    }

    struct ext2_inode *inode = kzalloc(sizeof(struct ext2_inode));
    struct ext2_full_inode *f = kzalloc(sizeof(struct ext2_full_inode));
    if (!f || !inode)
        return ERR_NO_MEM;

    if (!ext2_read_inode(fs, EXT2_ROOT_INODE, inode)) {
        return ERR_IO;
    }

    if (!out_node) {
        kfree(f);
        return ERR_OK;
    }

    memcpy(&f->node, inode, sizeof(struct ext2_inode));

    f->inode_num = EXT2_ROOT_INODE;
    kfree(inode);

    out_node->open_handles += 1;
    out_node->name[0] = '/';
    out_node->flags = ext2_to_vfs_flags(f->node.flags);
    out_node->mode = ext2_to_vfs_mode(f->node.mode);
    out_node->size = f->node.size;
    out_node->fs_data = fs;
    out_node->fs_node_data = f;
    out_node->fs_type = FS_EXT2;
    out_node->ops = &ext2_vfs_ops;
    return ERR_OK;
}

struct vfs_node *ext2_vfs_finddir(struct vfs_node *node, const char *fname) {
    struct ext2_full_inode *full_inode = node->fs_node_data;

    struct ext2_fs *fs = node->fs_data;

    struct ext2_full_inode *found =
        ext2_find_file_in_dir(fs, full_inode, fname, NULL);

    return make_vfs_node(node->fs_data, found, fname);
}

enum errno ext2_vfs_rename(struct vfs_node *old_parent, const char *old_name,
                           struct vfs_node *new_parent, const char *new_name) {
    if (!old_parent || !old_name || !new_name || !new_name)
        return ERR_INVAL;

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
    if (!parent || !target || !name)
        return ERR_INVAL;

    if (target->mode & VFS_MODE_DIR)
        return ERR_IS_DIR;

    // TODO: check if this is a directory
    struct ext2_full_inode *dir = parent->fs_node_data;
    struct ext2_full_inode *child = target->fs_node_data;
    struct ext2_fs *fs = parent->fs_data;

    return ext2_link_file(fs, dir, child, name, vfs_to_ext2_mode(target->mode));
}

enum errno ext2_vfs_symlink(struct vfs_node *parent, const char *target,
                            const char *link_name) {
    if (!parent || !target || !link_name)
        return ERR_INVAL;

    struct ext2_full_inode *dir = parent->fs_node_data;
    struct ext2_fs *fs = parent->fs_data;

    return ext2_symlink_file(fs, dir, link_name, target);
}

enum errno ext2_vfs_chmod(struct vfs_node *n, uint16_t mode) {
    if (!n)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    uint16_t new_mode = vfs_to_ext2_mode(mode);
    return ext2_chmod(fs, node, new_mode);
}

enum errno ext2_vfs_chown(struct vfs_node *n, uint32_t uid, uint32_t gid) {
    if (!n)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_chown(fs, node, uid, gid);
}

enum errno ext2_vfs_read(struct vfs_node *n, void *buf, uint64_t size,
                         uint64_t offset) {
    if (!n || !buf)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_read_file(fs, node, offset, buf, size);
}

enum errno ext2_vfs_write(struct vfs_node *n, const void *buf, uint64_t size,
                          uint64_t offset) {
    if (!n || !buf)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_write_file(fs, node, offset, buf, size);
}

enum errno ext2_vfs_utime(struct vfs_node *n, uint64_t atime, uint64_t mtime) {
    if (!n)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    struct ext2_inode *inode = &node->node;
    inode->atime = atime;
    inode->mtime = mtime;
    if (!ext2_write_inode(fs, node->inode_num, inode))
        return ERR_FS_INTERNAL;

    return ERR_OK;
}

enum errno ext2_vfs_truncate(struct vfs_node *n, uint64_t length) {
    if (!n)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_truncate_file(fs, node, length);
}

enum errno ext2_vfs_unlink(struct vfs_node *n, const char *name) {
    if (!n || !name)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_unlink_file(fs, node, name, true);
}

enum errno ext2_vfs_create(struct vfs_node *n, const char *name,
                           uint16_t mode) {
    if (!n || !name)
        return ERR_INVAL;

    uint16_t new_mode = vfs_to_ext2_mode(mode);
    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_create_file(fs, node, name, new_mode);
}

enum errno ext2_vfs_mkdir(struct vfs_node *n, const char *name, uint16_t mode) {
    if (!n || !name)
        return ERR_INVAL;

    uint16_t new_mode = vfs_to_ext2_mode(mode);
    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_mkdir(fs, node, name, new_mode);
}

enum errno ext2_vfs_rmdir(struct vfs_node *n, const char *name) {
    if (!n || !name)
        return ERR_INVAL;

    struct ext2_fs *fs = n->fs_data;
    struct ext2_full_inode *node = n->fs_node_data;
    return ext2_rmdir(fs, node, name);
}
