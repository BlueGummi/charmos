#include <compiler.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct vfs_node *tmpfs_create_vfs_node(struct tmpfs_node *tnode);

struct vfs_node *tmpfs_mkroot(const char *mount_point) {
    struct tmpfs_fs *fs = kzalloc(sizeof(struct tmpfs_fs));

    struct tmpfs_node *root = kzalloc(sizeof(struct tmpfs_node));
    if (!fs || !root)
        return false;

    root->type = TMPFS_DIR;
    root->name = strdup(mount_point);
    root->mode = 0755;

    fs->root = root;

    struct vfs_node *vnode = tmpfs_create_vfs_node(root);
    return vnode;
}

static uint16_t tmpfs_to_vfs_mode(enum tmpfs_type mode) {
    switch (mode) {
    case TMPFS_DIR: return VFS_MODE_DIR;
    case TMPFS_FILE: return VFS_MODE_FILE;
    case TMPFS_SYMLINK: return VFS_MODE_SYMLINK;
    }
    return -1;
}

static enum errno tmpfs_mount(struct vfs_node *mountpoint, struct vfs_node *out,
                              const char *name) {
    (void) mountpoint, (void) out, (void) name;
    return ERR_NOT_IMPL;
}

static enum errno tmpfs_read(struct vfs_node *node, void *buf, uint64_t size,
                             uint64_t offset) {
    struct tmpfs_node *tn = node->fs_node_data;

    if (tn->type != TMPFS_FILE)
        return ERR_IS_DIR;

    if (offset >= tn->size)
        return 0;

    if (offset + size > tn->size)
        size = tn->size - offset;

    uint8_t *out = buf;
    while (size > 0) {
        uint64_t page_idx = offset >> TMPFS_PAGE_SHIFT;
        uint64_t page_offset = offset & TMPFS_PAGE_MASK;
        uint64_t to_copy = TMPFS_PAGE_SIZE - page_offset;
        if (to_copy > size)
            to_copy = size;

        if (page_idx >= tn->num_pages || !tn->pages[page_idx]) {
            memset(out, 0, to_copy);
        } else {
            memcpy(out, (uint8_t *) tn->pages[page_idx] + page_offset, to_copy);
        }

        offset += to_copy;
        out += to_copy;
        size -= to_copy;
    }

    return ERR_OK;
}

static enum errno tmpfs_write(struct vfs_node *node, const void *buf,
                              uint64_t size, uint64_t offset) {
    struct tmpfs_node *tn = node->fs_node_data;
    if (tn->type != TMPFS_FILE)
        return ERR_IS_DIR;

    uint64_t end = offset + size;
    uint64_t required_pages = (end + TMPFS_PAGE_SIZE - 1) >> TMPFS_PAGE_SHIFT;

    if (required_pages > tn->num_pages) {
        void **new_pages = krealloc(tn->pages, required_pages * sizeof(void *));
        if (!new_pages)
            return ERR_NO_MEM;
        memset(new_pages + tn->num_pages, 0,
               (required_pages - tn->num_pages) * sizeof(void *));
        tn->pages = new_pages;
        tn->num_pages = required_pages;
    }

    const uint8_t *in = buf;
    while (size > 0) {
        uint64_t page_idx = offset >> TMPFS_PAGE_SHIFT;
        uint64_t page_offset = offset & TMPFS_PAGE_MASK;
        uint64_t to_copy = TMPFS_PAGE_SIZE - page_offset;
        if (to_copy > size)
            to_copy = size;

        if (!tn->pages[page_idx]) {
            tn->pages[page_idx] = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
            if (!tn->pages[page_idx])
                return ERR_NO_MEM;
            memset(tn->pages[page_idx], 0, TMPFS_PAGE_SIZE);
        }

        memcpy((uint8_t *) tn->pages[page_idx] + page_offset, in, to_copy);

        offset += to_copy;
        in += to_copy;
        size -= to_copy;
    }

    if (end > tn->size) {
        tn->size = end;
        node->size = end;
    }

    return ERR_OK;
}

static enum errno tmpfs_open(struct vfs_node *node, uint32_t flags) {
    (void) node, (void) flags;
    return ERR_NOT_IMPL; // no-op
}

static enum errno tmpfs_close(struct vfs_node *node) {
    (void) node;
    return ERR_NOT_IMPL; // no-op
}

static struct tmpfs_node *tmpfs_find_child(struct tmpfs_node *dir,
                                           const char *name) {
    for (uint64_t i = 0; i < dir->child_count; ++i)
        if (strcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    return NULL;
}

static enum errno tmpfs_add_child(struct tmpfs_node *parent,
                                  struct tmpfs_node *child) {
    uint64_t needed_size = sizeof(void *) * (parent->child_count + 1);

    parent->children = krealloc(parent->children, needed_size);

    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return ERR_OK;
}

static enum errno tmpfs_create_common(struct vfs_node *parent, const char *name,
                                      uint16_t mode, enum tmpfs_type type,
                                      struct tmpfs_node **out) {
    struct tmpfs_node *pt = parent->fs_node_data;
    if (pt->type != TMPFS_DIR)
        return ERR_NOT_DIR;

    if (tmpfs_find_child(pt, name))
        return ERR_EXIST;

    struct tmpfs_node *node = kzalloc(sizeof(*node));
    if (unlikely(!node))
        return ERR_NO_MEM;

    node->type = type;
    node->name = strdup(name);
    node->mode = mode;

    if (type == TMPFS_FILE) {
        node->pages = NULL;
        node->num_pages = 0;
        node->size = 0;
    }

    tmpfs_add_child(pt, node);
    if (out)
        *out = node;
    return ERR_OK;
}

static enum errno tmpfs_create(struct vfs_node *parent, const char *name,
                               uint16_t mode) {
    struct tmpfs_node *node;
    enum tmpfs_type type = (mode & VFS_MODE_DIR) ? TMPFS_DIR : TMPFS_FILE;
    return tmpfs_create_common(parent, name, mode, type, &node);
}

static enum errno tmpfs_mknod(struct vfs_node *parent, const char *name,
                              uint16_t mode, uint32_t dev) {
    (void) parent, (void) name, (void) mode, (void) dev;
    return ERR_NOT_IMPL;
}

static enum errno tmpfs_symlink(struct vfs_node *parent, const char *target,
                                const char *link_name) {
    struct tmpfs_node *link;
    enum errno err =
        tmpfs_create_common(parent, link_name, 0777, TMPFS_SYMLINK, &link);

    if (err != ERR_OK)
        return err;

    link->symlink_target = strdup(target);
    return ERR_OK;
}

static enum errno tmpfs_unmount(struct vfs_mount *mountpoint) {
    (void) mountpoint;
    return ERR_NOT_IMPL;
}

static enum errno tmpfs_stat(struct vfs_node *node, struct vfs_stat *out) {
    struct tmpfs_node *tn = node->fs_node_data;
    out->mode = tn->mode;
    out->size = tn->size;
    // could add uid/gid/mtime/etc.
    return ERR_OK;
}

static enum errno tmpfs_readdir(struct vfs_node *node, struct vfs_dirent *out,
                                uint64_t index) {
    struct tmpfs_node *tn = node->fs_node_data;
    if (tn->type != TMPFS_DIR)
        return ERR_NOT_DIR;

    if (index >= tn->child_count)
        return ERR_NO_ENT;

    struct tmpfs_node *child = tn->children[index];
    strncpy(out->name, child->name, sizeof(out->name));
    out->mode = tmpfs_to_vfs_mode(child->type);
    return ERR_OK;
}

static enum errno tmpfs_mkdir(struct vfs_node *parent, const char *name,
                              uint16_t mode) {
    return tmpfs_create_common(parent, name, mode, TMPFS_DIR, NULL);
}

static enum errno tmpfs_rmdir(struct vfs_node *parent, const char *name) {
    struct tmpfs_node *pt = parent->fs_node_data;
    for (uint64_t i = 0; i < pt->child_count; ++i) {
        struct tmpfs_node *c = pt->children[i];
        if (strcmp(c->name, name) == 0 && c->type == TMPFS_DIR) {
            // remove
            memmove(&pt->children[i], &pt->children[i + 1],
                    (pt->child_count - i - 1) * sizeof(void *));
            pt->child_count--;
            kfree(c->name);
            kfree(c->children);
            kfree(c);
            return ERR_OK;
        }
    }
    return ERR_NO_ENT;
}

static void tmpfs_free_node(struct tmpfs_node *tn) {
    for (uint64_t i = 0; i < tn->num_pages; ++i) {
        if (tn->pages[i]) {
            kfree_aligned(tn->pages[i]);
            tn->pages[i] = NULL;
        }
    }
    kfree(tn->pages);
}

static enum errno tmpfs_unlink(struct vfs_node *parent, const char *name) {
    struct tmpfs_node *pt = parent->fs_node_data;
    for (uint64_t i = 0; i < pt->child_count; ++i) {
        struct tmpfs_node *c = pt->children[i];
        if (strcmp(c->name, name) == 0 && c->type != TMPFS_DIR) {
            memmove(&pt->children[i], &pt->children[i + 1],
                    (pt->child_count - i - 1) * sizeof(void *));
            pt->child_count--;
            kfree(c->name);
            tmpfs_free_node(c);
            kfree(c);
            return ERR_OK;
        }
    }
    return ERR_NO_ENT;
}

static enum errno tmpfs_rename(struct vfs_node *old_parent,
                               const char *old_name,
                               struct vfs_node *new_parent,
                               const char *new_name) {
    struct tmpfs_node *old_pt = old_parent->fs_node_data;
    struct tmpfs_node *new_pt = new_parent->fs_node_data;
    struct tmpfs_node *node = tmpfs_find_child(old_pt, old_name);
    if (!node)
        return ERR_NO_ENT;

    for (uint64_t i = 0; i < old_pt->child_count; ++i) {
        if (old_pt->children[i] == node) {
            memmove(&old_pt->children[i], &old_pt->children[i + 1],
                    (old_pt->child_count - i - 1) * sizeof(void *));
            old_pt->child_count--;
            break;
        }
    }

    kfree(node->name);
    node->name = strdup(new_name);
    tmpfs_add_child(new_pt, node);
    return ERR_OK;
}

static enum errno tmpfs_truncate(struct vfs_node *node, uint64_t length) {
    struct tmpfs_node *tn = node->fs_node_data;
    if (tn->type != TMPFS_FILE)
        return ERR_IS_DIR;

    uint64_t old_size = tn->size;
    uint64_t new_pages = (length + TMPFS_PAGE_SIZE - 1) >> TMPFS_PAGE_SHIFT;

    if (new_pages < tn->num_pages) {
        for (uint64_t i = new_pages; i < tn->num_pages; ++i) {
            if (tn->pages[i]) {
                kfree_aligned(tn->pages[i]);
                tn->pages[i] = NULL;
            }
        }
    }

    if (new_pages != tn->num_pages) {
        void **new_array = krealloc(tn->pages, new_pages * sizeof(void *));
        if (!new_array && new_pages > 0)
            return ERR_NO_MEM;

        if (new_pages > tn->num_pages)
            memset(new_array + tn->num_pages, 0,
                   (new_pages - tn->num_pages) * sizeof(void *));

        tn->pages = new_array;
        tn->num_pages = new_pages;
    }

    if (length > old_size) {
        uint64_t last_page_idx = old_size >> TMPFS_PAGE_SHIFT;
        uint64_t last_offset = old_size & TMPFS_PAGE_MASK;

        if (last_page_idx < new_pages) {
            if (!tn->pages[last_page_idx]) {
                tn->pages[last_page_idx] = kmalloc(TMPFS_PAGE_SIZE);
                if (!tn->pages[last_page_idx])
                    return ERR_NO_MEM;
                memset(tn->pages[last_page_idx], 0, TMPFS_PAGE_SIZE);
            }

            uint64_t to_zero = TMPFS_PAGE_SIZE - last_offset;
            if (length - old_size < to_zero)
                to_zero = length - old_size;

            memset((uint8_t *) tn->pages[last_page_idx] + last_offset, 0,
                   to_zero);
        }
    }

    tn->size = length;
    node->size = length;
    return ERR_OK;
}

static enum errno tmpfs_readlink(struct vfs_node *node, char *buf,
                                 uint64_t size) {
    struct tmpfs_node *tn = node->fs_node_data;

    if (tn->type != TMPFS_SYMLINK)
        return ERR_IS_DIR;

    strncpy(buf, tn->symlink_target, size);
    return ERR_OK;
}

static enum errno tmpfs_link(struct vfs_node *parent, struct vfs_node *target,
                             const char *link_name) {
    (void) parent, (void) target, (void) link_name;
    // tmpfs doesn't support hard links
    return ERR_NOT_IMPL;
}

static enum errno tmpfs_chmod(struct vfs_node *node, uint16_t mode) {
    struct tmpfs_node *tn = node->fs_node_data;
    tn->mode = mode;
    node->mode = mode;
    return ERR_OK;
}

static enum errno tmpfs_chown(struct vfs_node *node, uint32_t uid,
                              uint32_t gid) {
    struct tmpfs_node *n = node->fs_node_data;
    node->uid = uid;
    node->gid = gid;
    n->uid = uid;
    n->gid = gid;
    return ERR_OK;
}

static enum errno tmpfs_utime(struct vfs_node *node, uint64_t atime,
                              uint64_t mtime) {
    struct tmpfs_node *n = node->fs_node_data;
    node->atime = atime;
    node->mtime = mtime;
    n->atime = atime;
    n->mtime = mtime;
    return ERR_OK;
}

static enum errno tmpfs_destroy(struct vfs_node *node) {
    (void) node;
    struct tmpfs_node *n = node->fs_node_data;

    if (!n)
        return ERR_INVAL;

    if (n->children)
        kfree(n->children);

    tmpfs_free_node(n);

    if (n->symlink_target)
        kfree(n->symlink_target);

    kfree(n);

    n->children = NULL;
    n->symlink_target = NULL;
    n = NULL;
    return ERR_OK;
}

static enum errno tmpfs_finddir(struct vfs_node *node, const char *name,
                                struct vfs_dirent *out) {
    struct tmpfs_node *tn = node->fs_node_data;

    if (tn->type != TMPFS_DIR)
        return ERR_NOT_DIR;

    struct tmpfs_node *child = tmpfs_find_child(tn, name);
    if (!child)
        return ERR_NO_ENT;

    struct vfs_node *n = tmpfs_create_vfs_node(child);
    struct vfs_dirent ent;
    ent.node = n;
    memcpy(ent.name, name, strlen(name));
    ent.mode = n->mode;
    ent.dirent_data = tn;

    memcpy(out, &ent, sizeof(struct vfs_dirent));

    return ERR_OK;
}

static const struct vfs_ops tmpfs_ops = {.read = tmpfs_read,
                                         .write = tmpfs_write,
                                         .open = tmpfs_open,
                                         .close = tmpfs_close,
                                         .create = tmpfs_create,
                                         .mknod = tmpfs_mknod,
                                         .symlink = tmpfs_symlink,
                                         .mount = tmpfs_mount,
                                         .unmount = tmpfs_unmount,
                                         .stat = tmpfs_stat,
                                         .readdir = tmpfs_readdir,
                                         .mkdir = tmpfs_mkdir,
                                         .rmdir = tmpfs_rmdir,
                                         .unlink = tmpfs_unlink,
                                         .rename = tmpfs_rename,
                                         .truncate = tmpfs_truncate,
                                         .readlink = tmpfs_readlink,
                                         .link = tmpfs_link,
                                         .chmod = tmpfs_chmod,
                                         .chown = tmpfs_chown,
                                         .utime = tmpfs_utime,
                                         .destroy = tmpfs_destroy,
                                         .finddir = tmpfs_finddir};

struct vfs_node *tmpfs_create_vfs_node(struct tmpfs_node *tnode) {
    struct vfs_node *vnode = kzalloc(sizeof(struct vfs_node));
    if (!vnode || !tnode)
        return NULL;

    vnode->mode = tnode->mode;
    vnode->size = tnode->size;

    vnode->fs_data = NULL; // could be fs pointer if needed
    vnode->fs_node_data = tnode;
    vnode->ops = &tmpfs_ops;
    vnode->fs_type = FS_TMPFS;
    return vnode;
}
