#include <errno.h>
#include <stddef.h>
#include <vfs/vfs.h>

struct vnode_ops {
    enum errno (*open)(struct vfs_node *vn);
    enum errno (*close)(struct vfs_node *vn);

    enum errno (*read)(struct vfs_node *vn, void *buf, size_t len,
                       off_t offset);
    enum errno (*write)(struct vfs_node *vn, const void *buf, size_t len,
                        off_t offset);

    enum errno (*mkdir)(struct vfs_node *vn, const char *name);
    enum errno (*unlink)(struct vfs_node *vn);
    enum errno (*lookup)(struct vfs_node *vn, const char *name,
                         struct vfs_node **result);
};

#pragma once
