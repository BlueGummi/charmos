#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum tmpfs_type {
    TMPFS_FILE,
    TMPFS_DIR,
    TMPFS_SYMLINK,
    // ...
};

struct tmpfs_node {
    enum tmpfs_type type;
    char *name;
    char *data;
    uint64_t size;

    char *symlink_target;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime;
    uint64_t atime;

    struct tmpfs_node *parent;

    struct tmpfs_node **children;
    size_t child_count;
};

struct tmpfs_fs {
    struct tmpfs_node *root;
};

struct vfs_node *tmpfs_mkroot(const char *mount_point);
