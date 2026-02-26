#include <log.h>
#include <console/printf.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>

#include "errno.h"
#include "fs/detect.h"

static struct vfs_mount **mount_table = NULL;
static uint64_t mount_table_count = 0;
static uint64_t mount_table_capacity = 0;

LOG_SITE_DECLARE_DEFAULT(vfs);
LOG_HANDLE_DECLARE_DEFAULT(vfs);

static void mount_table_add(struct vfs_mount *mnt) {
    if (mount_table_count == mount_table_capacity) {
        uint64_t new_capacity =
            (mount_table_capacity == 0) ? 4 : mount_table_capacity * 2;

        struct vfs_mount **new_table;
        new_table = krealloc(mount_table, new_capacity * sizeof(*mount_table),
                             ALLOC_PARAMS_DEFAULT);

        if (!new_table)
            return;
        mount_table = new_table;
        mount_table_capacity = new_capacity;
    }

    mount_table[mount_table_count++] = mnt;
}

static void mount_table_remove(struct vfs_mount *mnt) {
    for (uint64_t i = 0; i < mount_table_count; i++) {
        if (mount_table[i] == mnt) {
            mount_table[i] = mount_table[--mount_table_count];
            return;
        }
    }
}

void vfs_node_print(const struct vfs_node *node) {
    if (!node) {
        printf("vfs_node: (null)\n");
        return;
    }

    printf("=== VFS Node ===\n");
    printf("Type     : %s (%d)\n", detect_fstr(node->fs_type), node->fs_type);
    printf("Open     : %s\n", node->open_handles ? "Yes" : "No");
    printf("Flags    : 0x%08X\n", node->flags);
    printf("Mode     : 0%o\n", node->mode);
    printf("Size     : %llu bytes\n", (unsigned long long) node->size);
    printf("FS Data  : 0x%llx\n", node->fs_data);
    printf("FS Node  : 0x%llx\n", node->fs_node_data);
    printf("Ops Table: 0x%llx\n", (void *) node->ops);
    printf("=================\n");
}

static struct vfs_mount *find_mount_for_node(struct vfs_node *node) {
    for (uint64_t i = 0; i < mount_table_count; i++) {
        if (mount_table[i]->mount_point->unique_id == node->unique_id)
            return mount_table[i];
    }
    return NULL;
}

struct vfs_node *vfs_finddir(struct vfs_node *node, const char *fname) {
    if (!node || !fname)
        return NULL;

    struct vfs_mount *mnt = find_mount_for_node(node);
    if (mnt)
        node = mnt->root;

    if (!node->ops || !node->ops->finddir)
        return NULL;

    struct vfs_dirent ent = {0};

    node->ops->finddir(node, fname, &ent);

    struct vfs_node *found = ent.node;

    mnt = find_mount_for_node(found);
    if (mnt)
        return mnt->root;

    return found;
}

enum errno vfs_mount(struct vfs_node *mountpoint, struct vfs_node *target,
                     const char *name) {
    if (!mountpoint || !target)
        return ERR_INVAL;

    if (!(mountpoint->mode & VFS_MODE_DIR))
        return ERR_NOT_DIR;

    if (mountpoint->child_mount)
        return ERR_BUSY; // Already mounted here

    struct vfs_mount *mnt =
        kmalloc(sizeof(struct vfs_mount), ALLOC_PARAMS_DEFAULT);
    if (!mnt)
        return ERR_NO_MEM;

    memcpy(mnt->name, name, sizeof(mnt->name));
    mnt->root = target;
    mnt->mount_point = mountpoint;
    mnt->ops = target->ops;
    mnt->fs_data = target->fs_data;
    mnt->mount_point->child_mount = mnt;

    mount_table_add(mnt);
    return ERR_OK;
}

enum errno vfs_unmount(struct vfs_mount *mnt) {
    if (!mnt)
        return ERR_INVAL;

    if (mnt->mount_point)
        mnt->mount_point->child_mount = NULL;

    mount_table_remove(mnt);
    kfree(mnt, FREE_PARAMS_DEFAULT);
    return ERR_OK;
}

void vfs_clear_mounts(void) {
    for (uint64_t i = 0; i < mount_table_count; i++) {
        kfree(mount_table[i], FREE_PARAMS_DEFAULT);
    }
    kfree(mount_table, FREE_PARAMS_DEFAULT);
    mount_table = NULL;
    mount_table_count = 0;
    mount_table_capacity = 0;
}
