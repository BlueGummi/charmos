#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/ide.h>
#include <devices/nvme.h>
#include <devices/registry.h>
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

void registry_setup() {
    struct ide_drive *drives = kmalloc(sizeof(struct ide_drive) * 4);
    struct pci_device *devices;
    uint64_t count;
    pci_scan_devices(&devices, &count);
    for (uint64_t i = 0; i < count; i++) {
        struct pci_device dev = devices[i];
        if (dev.class_code == PCI_CLASS_MASS_STORAGE &&
            dev.subclass == PCI_SUBCLASS_NVM &&
            dev.prog_if == PCI_PROGIF_NVME) {
            struct nvme_device *d =
                nvme_discover_device(dev.bus, dev.device, dev.function);
            registry_register(nvme_create_generic(d));
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
                registry_register(d);
            }
        }
    }

    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        detect_fs(disk);
        disk->mount(disk);
    }

    // TODO: AHCI
}

void registry_print_devices() {
    for (uint64_t i = 0; i < disk_count; i++) {
        struct generic_disk *disk = registry_get_by_index(i);
        disk->print(disk);
        disk->print_fs(disk);
    }
}
