#include <stddef.h>
#include <vfs/vfs.h>

struct vnode_ops {
    int (*open)(struct vfs_node *vn);
    int (*close)(struct vfs_node *vn);
    size_t (*read)(struct vfs_node *vn, void *buf, size_t len, off_t offset);
    size_t (*write)(struct vfs_node *vn, const void *buf, size_t len, off_t offset);
    int (*mkdir)(struct vfs_node *vn, const char *name);
    int (*unlink)(struct vfs_node *vn);
    struct vfs_node* (*lookup)(struct vfs_node *vn, const char *name);
};

#pragma once