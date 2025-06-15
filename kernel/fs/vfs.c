#include <console/printf.h>
#include <fs/vfs.h>

const char *fs_type_to_string(enum fs_type type) {
    switch (type) {
    case FS_UNKNOWN: return "UNKNOWN";
    case FS_FAT32: return "FAT32";
    case FS_FAT16: return "FAT16";
    case FS_FAT12: return "FAT12";
    case FS_EXFAT: return "exFAT";
    case FS_EXT2: return "EXT2";
    case FS_EXT3: return "EXT3";
    case FS_EXT4: return "EXT4";
    case FS_NTFS: return "NTFS";
    case FS_ISO9660: return "ISO9660";
    default: return "INVALID";
    }
}

void vfs_node_print(const struct vfs_node *node) {
    if (!node) {
        k_printf("vfs_node: (null)\n");
        return;
    }

    k_printf("=== VFS Node ===\n");
    k_printf("Name     : %s%s\n", node->name,
             *node->name == '/' ? " (root)" : "");
    k_printf("Type     : %s (%d)\n", fs_type_to_string(node->type), node->type);
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
