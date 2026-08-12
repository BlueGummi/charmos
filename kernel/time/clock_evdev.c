#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <stdarg.h>
#include <string.h>
#include <time/clock_evdev.h>

#include "internal.h"

struct clock_evdev_group *clock_evdev_group_create(const char *name, ...) {
    struct clock_evdev_group *cedg =
        kmalloc(sizeof(struct clock_evdev_group), ALLOC_FLAGS_ZERO);

    if (!cedg)
        return NULL;

    INIT_LIST_HEAD(&cedg->clock_evdevs);
    va_list args;
    va_start(args, name);
    int ret = vasprintf(&cedg->name, name, args);
    va_end(args);

    ERR_HANDLE(ret, ERR_NO_MEM) {
        kfree(cedg);
        return NULL;
    }

    return cedg;
}

struct clock_evdev *clock_evdev_create(const char *name, ...) {
    struct clock_evdev *ced =
        kmalloc(sizeof(struct clock_evdev), ALLOC_FLAGS_ZERO);
    if (!ced)
        return NULL;

    if (!cpu_mask_init(&ced->cpu_mask, global.core_count)) {
        kfree(ced);
        return NULL;
    }

    va_list args;
    va_start(args, name);
    int ret = vasprintf(&ced->name, name, args);
    va_end(args);

    ERR_HANDLE(ret, ERR_NO_MEM) {
        cpu_mask_deinit(&ced->cpu_mask);
        kfree(ced);
        return NULL;
    }

    return ced;
}

void clock_evdev_register(struct clock_evdev *ced) {
    kassert(!ced->in_global_list);
    ced->in_global_list = true;
    locked_list_add(&clock_global.clock_evdevs, &ced->list_internal);
}

void clock_evdev_group_register(struct clock_evdev_group *cedg) {
    locked_list_add(&clock_global.clock_evdev_groups, &cedg->list_internal);
}

void clock_evdev_group_unregister(struct clock_evdev_group *cedg) {
    locked_list_del(&clock_global.clock_evdev_groups, &cedg->list_internal);
}

struct clock_evdev_group *clock_evdev_group_search_for(const char *name) {
    enum irql irql = locked_list_lock(&clock_global.clock_evdev_groups);

    struct clock_evdev_group *group = NULL;
    list_for_each_entry(group, &clock_global.clock_evdev_groups.list,
                        list_internal) {
        if (strcmp(group->name, name) == 0)
            goto out;
    }

out:
    locked_list_unlock(&clock_global.clock_evdev_groups, irql);
    return group;
}
