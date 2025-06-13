#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <string.h>

struct vfs_node *ext2_vfs_finddir(struct vfs_node *node, const char *fname);

static struct vfs_ops ext2_vfs_ops = {
    .finddir = ext2_vfs_finddir,

};

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

static enum vfs_node_type ext2_to_vfs_type(uint16_t mode) {
    switch (mode & EXT2_S_IFMT) {
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

static struct vfs_node *
make_vfs_node_from_ext2_node(struct ext2_fs *fs, struct ext2_full_inode *node,
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

struct vfs_node *ext2_vfs_finddir(struct vfs_node *node, const char *fname) {
    struct ext2_full_inode *full_inode =
        (struct ext2_full_inode *) &node->fs_node_data;

    struct ext2_fs *fs = (struct ext2_fs *) &node->fs_data;

    struct ext2_full_inode *found =
        ext2_find_file_in_dir(fs, full_inode, fname);

    return make_vfs_node_from_ext2_node(node->fs_data, found, fname);
}
