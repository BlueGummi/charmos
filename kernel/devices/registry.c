#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/registry.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <drivers/e1000.h>
#include <drivers/nvme.h>
#include <fs/detect.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <string.h>

struct disk_node {
    struct generic_disk *disk;
    struct disk_node *next;
};

static struct disk_node *disk_list = NULL;
static uint64_t disk_count = 0;

void registry_register(struct generic_disk *disk) {
    struct disk_node *node = kmalloc(sizeof(struct disk_node));
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
            kfree(old->disk);
            kfree(old);
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
    for (uint64_t i = 0; node && i < index; ++i)
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

void registry_setup() {
    struct ata_drive *drives = kmalloc(sizeof(struct ata_drive) * 4);
    struct pci_device *devices;
    uint64_t count;
    if (!drives)
        k_panic("Could not allocate space for devices\n");

    pci_scan_devices(&devices, &count);
    uint64_t nvme_cnt = 1, ahci_cnt = 1, ide_cnt = 1, atapi_cnt = 1;

    for (uint64_t i = 0; i < count; i++) {
        struct pci_device dev = devices[i];
        if (dev.class_code == 0x02 && dev.subclass == 0x00 &&
            dev.vendor_id == 0x8086) {
            switch (dev.device_id) {
            case 0x1000: // 82542
            case 0x100E: // 82540EM (QEMU default)
            case 0x1010: // 82546EB
            case 0x1026: // 82545EM
            case 0x10D3: // 82574L
            case 0x10F5: // 82567LM-3
                struct e1000_device *device =
                    kmalloc(sizeof(struct e1000_device));
                e1000_init(&dev, device);
                break;
            }
        }

        if (dev.class_code == PCI_CLASS_MASS_STORAGE &&
            dev.subclass == PCI_SUBCLASS_NVM &&
            dev.prog_if == PCI_PROGIF_NVME) {
            struct nvme_device *d =
                nvme_discover_device(dev.bus, dev.device, dev.function);
            struct generic_disk *disk = nvme_create_generic(d);
            device_mkname(disk, "nvme", nvme_cnt++);

            registry_register(disk);
            continue;
        }
        if (dev.class_code == 0x01 && dev.subclass == 0x06 &&
            dev.prog_if == 0x01) {
            uint32_t d_cnt = 0;
            struct ahci_disk *disks =
                ahci_discover_device(dev.bus, dev.device, dev.function, &d_cnt);

            for (uint32_t i = 0; i < d_cnt; i++) {
                struct generic_disk *disk = ahci_create_generic(&disks[i]);
                k_info("DEVICE", K_INFO, "Registering \"sata%u\"\n", ahci_cnt);
                device_mkname(disk, "sata", ahci_cnt++);
                registry_register(disk);
            }
            continue;
        }
    }

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int ind = i * 2 + j;
            if (ata_setup_drive(&drives[ind], devices, count, i, j)) {
                struct generic_disk *d = NULL;

                if (drives[ind].type == IDE_TYPE_ATA) {
                    d = ide_create_generic(&drives[ind]);
                    device_mkname(d, "ata", ide_cnt++);
                } else if (drives[ind].type == IDE_TYPE_ATAPI) {
                    d = atapi_create_generic(&drives[ind]);
                    device_mkname(d, "cdrom", atapi_cnt++);
                }

                if (!d)
                    continue;

                registry_register(d);
            }
        }
    }

    bool found_root = false;
    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        detect_fs(disk);
        for (uint32_t j = 0; j < disk->partition_count; j++) {
            struct generic_partition *p = &disk->partitions[j];

            if (strcmp(p->name, g_root_part) == 0) {
                struct vfs_node *root = p->mount(p);
                if (!root)
                    k_panic("VFS failed to mount root '%s' - mount failure\n",
                            g_root_part);
                g_root_node = root;

                root->ops->mkdir(root, "tmp", VFS_MODE_DIR);

                found_root = true;
            }
        }
    }
    if (!found_root)
        k_panic("VFS failed to mount root '%s' - could not find root\n",
                g_root_part);
}

void registry_print_devices() {
    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        k_printf("Disk %lu, \"" ANSI_GREEN "%s" ANSI_RESET
                 "\" is a %s. Filesystem(s):\n",
                 i, disk->name, get_generic_disk_str(disk->type));
        disk->print(disk);
        for (uint32_t j = 0; j < disk->partition_count; j++) {
            struct generic_partition *p = &disk->partitions[j];

            if (strcmp(p->name, g_root_part) != 0)
                p->mount(p); // Do not remount root
            p->print_fs(p);
        }
    }
}
