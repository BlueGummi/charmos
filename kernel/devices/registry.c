#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/registry.h>
#include <drivers/ahci.h>
#include <drivers/ide.h>
#include <drivers/nvme.h>
#include <fs/detect.h>
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
    if (counter == 0)
        counter_str[0] = '0';

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
    struct ide_drive *drives = kmalloc(sizeof(struct ide_drive) * 4);
    struct pci_device *devices;
    uint64_t count;
    pci_scan_devices(&devices, &count);
    uint64_t nvme_cnt = 0, ahci_cnt = 0, ide_cnt = 0;
    for (uint64_t i = 0; i < count; i++) {
        struct pci_device dev = devices[i];
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
                device_mkname(disk, "ahcisata", ahci_cnt++);

                registry_register(disk);
            }
            continue;
        }
    }

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int ind = i * 2 + j;
            if (ide_setup_drive(&drives[ind], devices, count, i, j)) {
                struct generic_disk *d = ide_create_generic(&drives[ind]);
                if (!d)
                    continue;
                device_mkname(d, "idesata", ide_cnt++);
                registry_register(d);
            }
        }
    }

    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        detect_fs(disk);

        disk->mount(disk);
    }
}

void registry_print_devices() {
    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        k_printf("Disk %lu, \"%s\" is a %s. Filesystem:\n", i, disk->name,
                 disk->type == G_IDE_DRIVE    ? "IDE DRIVE"
                 : disk->type == G_NVME_DRIVE ? "NVME DRIVE"
                                              : "AHCI DRIVE");
        disk->print_fs(disk);
    }
}
