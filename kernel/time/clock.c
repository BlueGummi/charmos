#include <cmdline.h>
#include <string.h>
#include <time/clock.h>

#include "internal.h"

struct clock_globals clock_global = {0};

void clocks_init() {
    locked_list_init(&clock_global.clock_evdevs, LOCKED_LIST_INIT_NORMAL);
    locked_list_init(&clock_global.clock_evdev_groups, LOCKED_LIST_INIT_NORMAL);
    locked_list_init(&clock_global.clocks, LOCKED_LIST_INIT_NORMAL);
}

struct clock *clock_create(const char *fmt, ...) {
    struct clock *clock = kmalloc(sizeof(struct clock), ALLOC_FLAGS_ZERO);
    if (!clock)
        return NULL;

    va_list args;
    va_start(args, fmt);
    int ret = vasprintf(&clock->name, fmt, args);
    va_end(args);

    ERR_HANDLE(ret, ERR_NO_MEM) {
        kfree(clock);
        return NULL;
    }

    return clock;
}
