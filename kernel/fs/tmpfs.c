#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <string.h>

struct vfs_node *tmpfs_create_vfs_node(struct tmpfs_node *tnode);

struct vfs_mount *tmpfs_mount(const char *mount_point) {
    struct tmpfs_fs *fs = kzalloc(sizeof(struct tmpfs_fs));

    struct tmpfs_node *root = kzalloc(sizeof(struct tmpfs_node));
    root->type = TMPFS_DIR;
    root->name[0] = '/';
    root->mode = 0755;

    fs->root = root;

    struct vfs_node *vnode = tmpfs_create_vfs_node(root);

    struct vfs_mount *mnt = kmalloc(sizeof(struct vfs_mount));
    memset(mnt, 0, sizeof(*mnt));
    mnt->root = vnode;
    mnt->fs = fs;
    strncpy(mnt->mount_point, mount_point, sizeof(mnt->mount_point));
    mnt->next = NULL;

    return mnt;
}

extern struct vfs_ops tmpfs_ops;

static uint16_t tmpfs_to_vfs_mode(enum tmpfs_type mode) {
    switch (mode) {
    case TMPFS_DIR: return VFS_MODE_DIR;
    case TMPFS_FILE: return VFS_MODE_FILE;
    case TMPFS_SYMLINK: return VFS_MODE_SYMLINK;
    }
    return -1;
}

struct vfs_node *tmpfs_create_vfs_node(struct tmpfs_node *tnode) {
    struct vfs_node *vnode = kmalloc(sizeof(struct vfs_node));
    memset(vnode, 0, sizeof(*vnode));
    vnode->type = tmpfs_to_vfs_mode(tnode->type);
    vnode->mode = tnode->mode;
    vnode->size = tnode->size;
    strncpy(vnode->name, tnode->name, sizeof(vnode->name));

    vnode->fs_data = NULL; // could be fs pointer if needed
    vnode->fs_node_data = tnode;
    vnode->ops = &tmpfs_ops;
    return vnode;
}
