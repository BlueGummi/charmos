#include <block/generic.h>
#include <charmos.h>
#include <console/printf.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <drivers/e1000.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <fs/detect.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <registry.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct disk_node {
    struct generic_disk *disk;
    struct disk_node *next;
};

static struct disk_node *disk_list = NULL;
static uint64_t disk_count = 0;

void registry_register(struct generic_disk *disk) {
    struct disk_node *node =
        kmalloc(sizeof(struct disk_node), ALLOC_PARAMS_DEFAULT);
    if (!node)
        return;

    node->disk = disk;
    node->next = disk_list;
    disk_list = node;
    disk_count++;
}

void registry_unregister(struct generic_disk *disk) {
    struct disk_node **indirect = &disk_list;
    while (*indirect) {
        if ((*indirect)->disk == disk) {
            struct disk_node *old = *indirect;
            *indirect = old->next;
            kfree(old->disk, FREE_PARAMS_DEFAULT);
            kfree(old, FREE_PARAMS_DEFAULT);
            disk_count--;
            return;
        }
        indirect = &(*indirect)->next;
    }
}

struct generic_disk *registry_get_by_name(const char *name) {
    for (struct disk_node *node = disk_list; node; node = node->next) {
        if (strcmp(node->disk->name, name) == 0)
            return node->disk;
    }
    return NULL;
}

struct generic_disk *registry_get_by_index(uint64_t index) {
    struct disk_node *node = disk_list;
    for (uint64_t i = 0; node && i < index; i++)
        node = node->next;
    return node ? node->disk : NULL;
}

uint64_t registry_get_disk_cnt(void) {
    return disk_count;
}

static char *mkname(char *prefix, uint64_t counter) {
    uint32_t n = 0;
    char counter_str[25] = {0};
    do {
        counter_str[n++] = '0' + (counter % 10);
        counter /= 10;
    } while (counter > 0);
    char *cat = strcat(prefix, counter_str);
    return cat;
}

static void device_mkname(struct generic_disk *disk, const char *prefix,
                          uint64_t counter) {
    char diff_prefix[16] = {0};
    memcpy(diff_prefix, prefix, strlen(prefix));
    char *name = mkname(diff_prefix, counter);
    char fmtname[16] = {0};
    memcpy(fmtname, name, 16);
    memcpy(disk->name, fmtname, 16);
}

void registry_mkname(struct generic_disk *disk, const char *prefix,
                     uint64_t counter) {
    device_mkname(disk, prefix, counter);
}

void registry_setup() {
    struct pci_device *devices;
    uint64_t count;

    pci_scan_devices(&devices, &count);
    k_info("PCI", K_INFO, "Found %u devices", count);

    pci_init_devices(devices, count);
    ata_init(devices, count);

    k_info("VFS", K_INFO, "Attempting to find and mount root '%s'",
           global.root_partition);

    bool found_root = false;
    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        detect_fs(disk);
        for (uint32_t j = 0; j < disk->partition_count; j++) {
            struct generic_partition *p = &disk->partitions[j];

            if (strcmp(p->name, global.root_partition) == 0) {
                struct vfs_node *root = p->mount(p);
                if (!root)
                    k_panic("VFS failed to mount root '%s' - mount failure\n",
                            global.root_partition);
                global.root_node = root;
                global.root_node_disk = disk;
                found_root = true;
            }
        }
    }

    if (!found_root)
        k_panic("VFS failed to mount root '%s' - could not find root\n",
                global.root_partition);

    k_info("VFS", K_INFO, "Root '%s' mounted - is a(n) %s filesystem",
           global.root_partition, detect_fstr(global.root_node->fs_type));
}
