#include <mem/alloc.h>
#include <console/printf.h>
#include <stdint.h>
#include <string.h>
#include <vfs/ops.h>
#include <vfs/vfs.h>

static struct vfs_node *root = NULL;

static struct vnode_ops dummy_ops = {
    .read = vfs_read,
};

uint64_t vfs_read(struct vfs_node *node, void *buf, size_t size,
                  size_t offset) {
    if (!node || !buf || node->type != VFS_FILE || offset >= node->size)
        return 0;

    char *data = (char *) node->internal_data;
    size_t remaining = node->size - offset;
    size_t to_copy = (size < remaining) ? size : remaining;

    memcpy(buf, data + offset, to_copy);
    return to_copy;
}

uint64_t vfs_write(struct vfs_node *node, const void *buf, size_t size,
                   size_t offset) {
    if (!node || !buf || node->type != VFS_FILE)
        return 0;

    size_t required_size = offset + size;

    if (required_size > node->size) {
        void *new_data = kmalloc(required_size);
        if (!new_data)
            return 0;

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
    return size;
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
    node->ops = &dummy_ops;
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

int vfs_lookup(struct vfs_node *dir, const char *name,
               struct vfs_node **result) {
    if (!dir || !name || !result || dir->type != VFS_DIRECTORY)
        return -1;

    if (!dir->ops || !dir->ops->lookup)
        return -2;

    return dir->ops->lookup(dir, name, result);
}

struct vfs_node *vfs_resolve_path(const char *path) {
    if (!path || path[0] != '/')
        return NULL;

    struct vfs_node *current = root;
    char component[256];

    path++;

    while (*path) {
        size_t len = 0;
        while (*path && *path != '/') {
            component[len++] = *path++;
        }
        component[len] = '\0';

        if (*path == '/')
            path++;

        struct vfs_node *next = NULL;
        if (vfs_lookup(current, component, &next) != 0) {
            return NULL;
        }

        current = next;
    }

    return current;
}
