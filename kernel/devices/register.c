#include <devices/generic_disk.h>
#include <mem/alloc.h>

static struct generic_disk **disk_registry = NULL;
static uint64_t disk_count = 0;

void register_device(struct generic_disk *disk) {
    if (disk_count == 0) {
        disk_registry = kmalloc(sizeof(struct generic_disk *));
    } else {
        disk_registry = krealloc(disk_registry, sizeof(struct generic_disk *) *
                                                    (disk_count + 1));
    }
    disk_registry[disk_count] = disk;
    disk_count++;
}
