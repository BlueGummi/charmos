#include <cmdline.h>
#include <mem/alloc.h>
#include <string.h>
#include <structures/locked_list.h>
#include <time/clock.h>
#include <time/names.h>

#include "internal.h"

struct clock_globals clock_global = {0};

void clocks_init(void) {
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
    int ret = ERR_GUARD(vasprintf(&clock->name, fmt, args), ERR_NO_MEM);
    va_end(args);

    if (ret == ERR_NO_MEM) {
        kfree(clock);
        return NULL;
    }

    return clock;
}

void clock_register(struct clock *c) {
    locked_list_add(&clock_global.clocks, &c->list_internal);
}

void clock_unregister(struct clock *c) {
    locked_list_del(&clock_global.clocks, &c->list_internal);
}

/* TODO: If/When we start we start getting actual callers that use dynamic
 * lock registration, we'll need to refcount the clocks */
struct clock *clock_get_best(void) {
    struct clock *best = NULL;
    enum clock_rating best_rating = CLOCK_RATING_UNSUITABLE;

    struct list_head *pos;
    enum irql irql = spin_lock(&clock_global.clocks.lock);
    list_for_each(pos, &clock_global.clocks.list) {
        struct clock *c = container_of(pos, struct clock, list_internal);
        if (c->state == CLOCK_STATE_ON && !(c->flags & CLOCK_FLAG_UNSTABLE)) {
            if (c->rating > best_rating) {
                best_rating = c->rating;
                best = c;
            }
        }
    }
    spin_unlock(&clock_global.clocks.lock, irql);

    return best;
}

struct clock *clock_get_by_name(const char *name) {
    struct clock *found = NULL;
    struct list_head *pos;
    enum irql irql = spin_lock(&clock_global.clocks.lock);
    list_for_each(pos, &clock_global.clocks.list) {
        struct clock *c = container_of(pos, struct clock, list_internal);
        if (strcmp(c->name, name) == 0) {
            found = c;
            break;
        }
    }
    spin_unlock(&clock_global.clocks.lock, irql);
    return found;
}
