#pragma once

#include <devices/generic_disk.h>
void registry_register(struct generic_disk *disk);
void registry_unregister(struct generic_disk *disk);
struct generic_disk *registry_get_by_name(const char *name);
struct generic_disk *registry_get_by_index(uint64_t index);
uint64_t registry_get_disk_cnt(void);
void registry_setup();
void registry_print_devices();
void registry_detect_fs();
