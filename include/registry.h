/* @title: Registry */
#pragma once
#include <log.h>
#include <stdint.h>

struct generic_disk;
void registry_register(struct generic_disk *disk);
void registry_unregister(struct generic_disk *disk);
struct generic_disk *registry_get_by_name(const char *name);
struct generic_disk *registry_get_by_index(uint64_t index);
uint64_t registry_get_disk_cnt(void);
void registry_setup();
void registry_mkname(struct generic_disk *disk, const char *prefix,
                     uint64_t counter);

#define k_print_register(name)                                                 \
    log_msg(LOG_INFO, "Registering " ANSI_GREEN "%s" ANSI_RESET, name)
