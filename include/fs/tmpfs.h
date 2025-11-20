#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/mutex.h>

enum tmpfs_type {
    TMPFS_FILE,
    TMPFS_DIR,
    TMPFS_SYMLINK,
    // ...
};

struct tmpfs_node {
    enum tmpfs_type type;
    char *name;

    void **pages;       // array of page pointers
    uint64_t num_pages; // number of allocated pages
    uint64_t size;      // total file size

    char *symlink_target;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime;
    uint64_t atime;

    struct tmpfs_node *parent;

    struct tmpfs_node **children;
    uint64_t child_count;
    struct mutex lock;
};

struct tmpfs_fs {
    struct tmpfs_node *root;
};

#define TMPFS_PAGE_SIZE 4096
#define TMPFS_PAGE_SHIFT 12
#define TMPFS_PAGE_MASK (TMPFS_PAGE_SIZE - 1)

struct vfs_node *tmpfs_mkroot(const char *mount_point);
