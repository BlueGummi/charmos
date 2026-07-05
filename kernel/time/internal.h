#include <structures/locked_list.h>
#include <time/clock.h>
#include <time/clock_evdev.h>
#include <time/time.h>

struct clock_globals {
    struct locked_list clocks;
    struct locked_list clock_evdevs;
    struct locked_list clock_evdev_groups;
};
