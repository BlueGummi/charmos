#include <printf.h>
#include <stdint.h>
#include <string.h>
#include <vfs/ops.h>
#include <vfs/vfs.h>
#include <vmalloc.h>

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
    node->parent = NULL;
    node->children = NULL;
    node->ops = &dummy_ops;
    node->internal_data = data;
    node->ref_count = 1;
    node->is_mountpoint = false;

    return node;
}

void vfs_delete_node(struct vfs_node *node) {
    kfree(node->name, strlen(node->name) + 1);
    kfree(node, sizeof(struct vfs_node));
}

// TODO: allocate more space if the parent has ran out of space to hold children
void vfs_add(struct vfs_node *parent, struct vfs_node *child) {
    if (!parent || !child)
        return;

    if (parent->type != VFS_DIRECTORY) // Why are you adding children to a file
        return;

    if (parent->children == NULL) {
        parent->children = kmalloc(sizeof(struct vfs_node *) * 2);
        parent->children[0] = child;
        parent->children[1] = NULL;
        parent->child_count++;
    } else {
        // TODO: we should make the children a list of pointers that are arrays
        // of pointers to files, so that reallocation problems will not be an
        // issue
        parent->children[parent->child_count++] = child;
        parent->children[parent->child_count] = NULL;
    }
    child->parent = parent;
}

uint64_t vfs_find_child_index(struct vfs_node *parent, struct vfs_node *child) {
    uint64_t child_index = UINT64_MAX;
    for (uint64_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            child_index = i;
            break;
        }
    }
    return child_index;
}

void vfs_remove(struct vfs_node *parent, struct vfs_node *child) {
    if (!parent || !child)
        return;

    if (parent->type != VFS_DIRECTORY)
        return; // We are only removing files in directories

    uint64_t child_index = vfs_find_child_index(parent, child);

    if (child_index == UINT64_MAX) {
        return;
    }

    for (uint64_t i = child_index; i < parent->child_count - 1; i++) {
        // Everyone over one to the left
        parent->children[i] = parent->children[i + 1];
    }
    parent->children[--parent->child_count] = NULL;
    vfs_delete_node(child);
}

void vfs_remove_recurse(struct vfs_node *parent, struct vfs_node *child) {
    if (!parent || !child)
        return;
    
    uint64_t child_index = vfs_find_child_index(parent, child);

    if (child_index == UINT64_MAX) {
        return;
    }

    if (child->type == VFS_DIRECTORY) {
        for (uint64_t i = 0; i < child->child_count; i++) {
            vfs_remove_recurse(child, child->children[i]);
        }
        vfs_remove(parent, child);
    } else {
        // File
        vfs_remove(parent, child);
    }

}

void vfs_init() {
    root = vfs_create_node("/", VFS_DIRECTORY, 0, NULL);
}

struct vfs_node *vfs_lookup(const char *name) {
    if (!root || !name)
        return NULL;

    struct vfs_node **children = root->children;

    if (!children)
        return NULL;

    for (int i = 0; children[i] != NULL; i++) {
        if (strcmp(children[i]->name, name) == 0)
            return children[i];
    }
    return NULL;
}

void debug_print_fs(struct vfs_node *node, int depth) {
    if (!node)
        return;

    for (int i = 0; i < depth; i++)
        k_printf("  ");

    k_printf("%s (%s)\n", node->name,
             node->type == VFS_DIRECTORY ? "dir" : "file");

    if (node->type == VFS_DIRECTORY && node->children) {
        for (int i = 0; node->children[i]; i++) {
            debug_print_fs(node->children[i],
                           depth + 1); // recursion ok here - this for tests
        }
    }
}

void lookie() {
    struct vfs_node *file = vfs_lookup("test.txt");
    if (!file) {
        k_printf("test.txt not found.\n");
        return;
    }

    char buf[20] = {0};
    uint64_t bytes = file->ops->read(file, buf, sizeof(buf) - 1, 0);

    if (bytes > 0) {
        buf[bytes] = '\0';
        k_printf("Read %llu bytes: '%s'\n", bytes, buf);
    }
}

void read_test() {

    struct vfs_node *new_file =
        vfs_create_node("test.txt", VFS_FILE, 11, "MUSTAAARD");
    
    
    struct vfs_node *new_file_2 =
        vfs_create_node("ketchup.txt", VFS_FILE, 11, "KETCHUUPPP");

    struct vfs_node *new_dir =
        vfs_create_node("try_it", VFS_DIRECTORY, 11, "");
    vfs_add(root, new_dir);
    vfs_add(new_dir, new_file);
    vfs_add(new_dir, new_file_2);
    debug_print_fs(root, 0);
    vfs_remove_recurse(root, new_dir);
    debug_print_fs(root, 0);
}
