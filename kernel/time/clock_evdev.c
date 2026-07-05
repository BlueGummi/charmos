#include <mem/alloc.h>

#include "internal.h"

struct clock_evdev_group *clock_evdev_group_create(const char *name, ...) {
    struct clock_evdev_group *cedg = kmalloc(sizeof(struct clock_evdev_group));
    INIT_LIST_HEAD(&cedg->clock_evdevs);
    return cedg;
}
