#include <console/printf.h>
#include <fs/vfs.h>
#include <mem/alloc.h>

void vfs_node_print(const struct vfs_node *node) {
    if (!node) {
        k_printf("vfs_node: (null)\n");
        return;
    }

    k_printf("=== VFS Node ===\n");
    k_printf("Name     : %s%s\n", node->name,
             *node->name == '/' ? " (root)" : "");
    k_printf("Type     : %s (%d)\n", detect_fstr(node->fs_type), node->fs_type);
    k_printf("Open     : %s\n", node->open ? "Yes" : "No");
    k_printf("Flags    : 0x%08X\n", node->flags);
    k_printf("Mode     : 0%o\n", node->mode);
    k_printf("Size     : %llu bytes\n", (unsigned long long) node->size);
    k_printf("FS Data  : 0x%llx\n", node->fs_data);
    k_printf("FS Node  : 0x%llx\n", node->fs_node_data);
    k_printf("Ops Table: 0x%llx\n", (void *) node->ops);
    k_printf("=================\n");
}

struct vfs_node *vfs_finddir(struct vfs_node *node, const char *fname) {
    if (node->mount) {
        node = node->mount->root;
    }

    if (!node->ops || !node->ops->finddir)
        return NULL;

    struct vfs_node *found = node->ops->finddir(node, fname);

    // check for mount redirection on the found node
    if (found && found->mount) {
        return found->mount->root;
    }

    return found;
}

enum errno vfs_mount(struct vfs_node *mountpoint, struct vfs_node *target) {
    if (!mountpoint || !target)
        return ERR_INVAL;

    if (!(mountpoint->mode & VFS_MODE_DIR))
        return ERR_NOT_DIR;

    struct vfs_mount *mnt = kmalloc(sizeof(struct vfs_mount));
    mnt->root = target;
    mnt->mount_point = mountpoint;
    mnt->ops = target->ops;
    return ERR_OK;
}

enum errno vfs_unmount(struct vfs_mount *mountpoint) {
    if (!mountpoint)
        return ERR_INVAL;
    mountpoint->mount_point->mount = NULL;
    // TODO: free
    mountpoint->ops = NULL;
    kfree(mountpoint);
    return ERR_OK;
}
