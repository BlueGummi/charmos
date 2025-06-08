#include <console/printf.h>
#include <err.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <vfs/ops.h>
#include <vfs/vfs.h>

// FIXME: all this is placeholder - once I finish FAT I will fix

static struct vfs_node *root = NULL;

enum errno vfs_read(struct vfs_node *node, void *buf, size_t size,
                    size_t offset) {
    if (!node || !buf || node->type != VFS_FILE || offset >= node->size)
        return ERR_INVAL;

    char *data = (char *) node->internal_data;
    size_t remaining = node->size - offset;
    size_t to_copy = (size < remaining) ? size : remaining;

    memcpy(buf, data + offset, to_copy);
    return ERR_OK;
}

enum errno vfs_write(struct vfs_node *node, const void *buf, size_t size,
                     size_t offset) {
    if (!node || !buf || node->type != VFS_FILE)
        return ERR_INVAL;

    size_t required_size = offset + size;

    if (required_size > node->size) {
        void *new_data = kmalloc(required_size);
        if (!new_data)
            return ERR_FS_INTERNAL;

        if (node->internal_data) {
            memcpy(new_data, node->internal_data, node->size);
            kfree(node->internal_data);
        }

        if (offset > node->size) {
            memset((char *) new_data + node->size, 0, offset - node->size);
        }

        node->internal_data = new_data;
        node->size = required_size;
    }

    memcpy((char *) node->internal_data + offset, buf, size);
    return ERR_OK;
}

struct vfs_node *vfs_create_node(const char *name, enum vnode_type type,
                                 uint64_t size, void *data) {
    struct vfs_node *node = kmalloc(sizeof(struct vfs_node));
    if (!node)
        return NULL;

    node->name = kmalloc(strlen(name) + 1);
    strcpy(node->name, name);

    node->type = type;
    node->permissions = 0;
    node->uid = 0;
    node->gid = 0;
    node->size = size;
    node->inode = 0;
    node->internal_data = data;
    node->ref_count = 1;
    node->is_mountpoint = false;

    return node;
}

void vfs_delete_node(struct vfs_node *node) {
    if (!node)
        return;

    kfree(node->name);
    kfree(node);
}

void vfs_init() {
    root = vfs_create_node("/", VFS_DIRECTORY, 0, NULL);
}

enum errno vfs_lookup(struct vfs_node *dir, const char *name,
                      struct vfs_node **result) {
    if (!dir || !name || !result || dir->type != VFS_DIRECTORY)
        return ERR_INVAL;

    if (!dir->ops || !dir->ops->lookup)
        return ERR_INVAL;

    return dir->ops->lookup(dir, name, result);
}
