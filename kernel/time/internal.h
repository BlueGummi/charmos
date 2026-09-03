#include <structures/bitmap.h>
#include <structures/hlist.h>
#include <structures/locked_list.h>
#include <time/clock.h>
#include <time/clock_evdev.h>
#include <time/time.h>

enum timer_base_type {
    TIMER_BASE_LOCAL,
    TIMER_BASE_GLOBAL,
    TIMER_BASE_DEFERRED,
    TIMER_BASE_MAX
};

#define TIMER_LEVELS 10
#define TIMER_LEVEL_BITS 6
#define TIMER_LEVEL_SIZE (1ULL << TIMER_LEVEL_BITS)
#define TIMER_LEVEL_MASK (TIMER_LEVEL_SIZE - 1)
#define TIMER_LEVEL_OFFSET(n) ((n) * TIMER_LEVEL_SIZE)

#define TIMER_CLOCK_SHIFT 4
#define TIMER_CLOCK_FACTOR (1ULL << TIMER_CLOCK_SHIFT)
#define TIMER_CLOCK_MASK (TIMER_CLOCK_FACTOR - 1)

#define TIMER_LEVEL_SHIFT(n) ((n) * TIMER_CLOCK_SHIFT)
#define TIMER_LEVEL_GRANULARITY(n) (1ULL << TIMER_LEVEL_SHIFT(n))
#define TIMER_LEVEL_START(n)                                                   \
    ((TIMER_LEVEL_SIZE - 1) << (((n) - 1) * TIMER_CLOCK_SHIFT))
#define TIMER_LEVEL_END(n) (TIMER_LEVEL_START(n + 1) - 1)

#define TIMER_WHEEL_SIZE (TIMER_LEVELS * TIMER_LEVEL_SIZE)
#define TIMER_WHEEL_TIMEOUT_CUTOFF (TIMER_LEVEL_START(TIMER_LEVELS))
#define TIMER_WHEEL_TIMEOUT_MAX                                                \
    (TIMER_WHEEL_TIMEOUT_CUTOFF - TIMER_LEVEL_GRANULARITY(TIMER_LEVELS - 1))

#define TIMER_SYNC_SPIN_TIMES 500

/*
 * Since we don't have jiffies, the finest granularity we work with is
 * a single microsecond, and this is a non-cascading implementation,
 * resulting in this hierarchy (wide table, so we turn clang-format off):
 */

// clang-format off

/*
 * Level Offset        Granularity                     Range
 *   0     0              1 us                      0 us -                63 us
 *   1    64             16 us                     64 us -             1,023 us (~64us  - ~1ms)
 *   2   128            256 us                  1,024 us -            16,383 us (~1ms   - ~16ms)
 *   3   192          4,096 us (~4ms)          16,384 us -           262,143 us (~16ms  - ~262ms)
 *   4   256         65,536 us (~65ms)        262,144 us -         4,194,303 us (~262ms - ~4s)
 *   5   320      1,048,576 us (~1s)        4,194,304 us -        67,108,863 us (~4s    - ~1m)
 *   6   384     16,777,216 us (~16s)      67,108,864 us -     1,073,741,823 us (~1m    - ~17m)
 *   7   448    268,435,456 us (~4m)    1,073,741,824 us -    17,179,869,183 us (~17m   - ~5h)
 *   8   512  4,294,967,296 us (~1h)   17,179,869,184 us -   274,877,906,943 us (~5h    - ~3d)
 *   9   576 68,719,476,736 us (~19h) 274,877,906,944 us - 4,398,046,511,103 us (~3d    - ~50d)
 */

// clang-format on

/* Otherwise there won't be enough room in `timer_flags` */
static_assert(TIMER_WHEEL_SIZE <= 1024);

struct timer_base {
    enum timer_base_type type;
    cpu_id_t cpu;
    struct spinlock lock;
    struct timer *running;
    struct timer_percpu *percpu;
    time_us_t clock;              /* Updated before enqueue */
    time_us_t next_expiration_us; /* Expiration ts of the next timer */
    bool idle;
    bool pending; /* Any timers pending? */
    bool next_expiration_recalc;

    BITMAP_DECLARE(pending_map, TIMER_WHEEL_SIZE);
    struct hlist_head buckets[TIMER_WHEEL_SIZE];
};

struct timer_percpu {
    struct timer_base bases[TIMER_BASE_MAX];
    struct dpc timer_dpc;

    struct spinlock lock; /* THIS one is for the dpc_timers */
    struct hlist_head dpc_timers;
    struct clock_evdev *active_evdev;
};

struct clock_globals {
    struct locked_list clocks;
    struct locked_list clock_evdevs;
    struct locked_list clock_evdev_groups;
    char *timer_clock_evdev;
};

extern struct clock_globals clock_global;
void timer_base_reprogram_hardware(cpu_id_t cpu);

struct clock *hpet_clock_init(void);

void timekeeper_init(void);
void timekeeper_update(void);
time_ns_t timekeeper_get_ns(void);

#define TIMEKEEPER_TRY_READ_SPINS 4
bool timekeeper_try_get_ns(time_ns_t *out);
time_us_t timekeeper_get_us(void);

struct clock *clock_get_best(void);
struct clock *clock_get_by_name(const char *name);
