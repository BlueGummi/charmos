#include <printf.h>
#include <stdint.h>
#include <string.h>
#include <vfs.h>
#include <vmalloc.h>

static struct vfs_node *root = NULL;

uint64_t memfs_read(struct vfs_node *node, void *buf, size_t size,
                    size_t offset) {
    if (offset >= node->size)
        return 0;

    size_t remaining = node->size - offset;
    size_t to_copy = (size < remaining) ? size : remaining;

    memcpy(buf, (char *) node->data + offset, to_copy);
    return to_copy;
}

void vfs_init() {
    root = vmm_alloc_pages(1);
    strcpy(root->name, "/");
    root->flags = VFS_DIRECTORY;
    root->children = NULL;

    struct vfs_node *file = vmm_alloc_pages(1);
    strcpy(file->name, "test.txt");
    file->flags = VFS_FILE;
    file->size = 12;
    file->read = &memfs_read;
    file->data = "Hello World";

    file->next_sibling = root->children;
    root->children = file;

    struct vfs_node *home = vmm_alloc_pages(1);
    strcpy(home->name, "home");
    home->flags = VFS_DIRECTORY;
    home->children = NULL;

    home->next_sibling = file;
    root->children = home;
}

struct vfs_node *vfs_lookup(const char *name) {
    if (!root || !name)
        return NULL;

    struct vfs_node *curr = root->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next_sibling;
    }
    return NULL;
}

void debug_print_fs(struct vfs_node *node, int depth) {
    for (int i = 0; i < depth; i++)
        k_printf("  ");
    k_printf("%s (%s)\n", node->name,
             (node->flags & VFS_DIRECTORY) ? "dir" : "file");

    if (node->flags & VFS_DIRECTORY) {
        struct vfs_node *child = node->children;
        while (child) {
            debug_print_fs(child, depth + 1);
            child = child->next_sibling;
        }
    }
}

void read_test() {
    k_printf("Filesystem structure:\n");
    debug_print_fs(root, 0);

    struct vfs_node *file = vfs_lookup("test.txt");

    char buf[20] = {0};
    uint64_t bytes = memfs_read(file, buf, sizeof(buf) - 1, 0);

    if (bytes > 0) {
        buf[bytes] = '\0';
        k_printf("Read %zd bytes: '%s'\n", bytes, buf);
    }
}
