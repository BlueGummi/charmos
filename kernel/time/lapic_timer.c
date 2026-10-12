#include <acpi/lapic.h>
#include <global.h>
#include <irq/irq.h>
#include <math/align.h>
#include <smp/core.h>
#include <time/clock_evdev.h>
#include <time/lapic_timer.h>
#include <time/names.h>
#include <time/spin_sleep.h>

void lapic_timer_init(cpu_id_t core_id) {
    uint32_t calibration_sleep_ms = 2;

    lapic_write(LAPIC_REG_SVR, LAPIC_ENABLE | 0xFF);
    lapic_write(LAPIC_REG_TIMER_DIV, 0b0011);
    lapic_write(LAPIC_REG_LVT_TIMER,
                IRQ_TIMER | LAPIC_LVT_MASK | TIMER_MODE_ONESHOT);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

    sleep_spin_ms(calibration_sleep_ms);

    uint32_t curr = lapic_read((LAPIC_REG_TIMER_CUR));
    uint32_t elapsed = 0xFFFFFFFF - curr;

    freq_khz_t lapic_calibrated_khz = elapsed / calibration_sleep_ms;

    global.cores[core_id]->lapic_khz = lapic_calibrated_khz;
    lapic_timer_disable();
}

void lapic_timer_init_bsp(void) {
    lapic_timer_init(0);
}

void lapic_timer_disable(void) {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    lvt |= LAPIC_LVT_MASK;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);
}

void lapic_timer_enable(void) {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    lvt &= ~LAPIC_LVT_MASK;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);
}

bool lapic_timer_is_enabled(void) {
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    return !BIT_TEST(lvt, 16);
}

static enum err lapic_evdev_set_next_event(struct clock_evdev *ced,
                                           time_ns_t delta_ns) {
    cc_unused(ced);

    /* The timer context guarantees that set_next_event happens under HIGH */
    freq_khz_t freq_khz = smp_core(TOPC_IRQL)->lapic_khz;
    uint64_t ticks = (freq_khz * (uint64_t) delta_ns) / 1000000ULL;

    if (ticks == 0)
        ticks = 1;

    if (ticks > 0xFFFFFFFFULL)
        ticks = 0xFFFFFFFFULL;

    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
    lvt &= ~(TIMER_MODE_PERIODIC | LAPIC_LVT_MASK);
    lvt |= (IRQ_TIMER | TIMER_MODE_ONESHOT);
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);

    /* Oneshot countdown */
    lapic_write(LAPIC_REG_TIMER_INIT, (uint32_t) ticks);

    return 0;
}

static enum err lapic_evdev_change_state(struct clock_evdev *ced,
                                         enum clock_evdev_state state) {
    cc_unused(ced);
    uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);

    switch (state) {
    case CLOCK_EVDEV_STATE_ONESHOT:
        lvt &= ~(TIMER_MODE_PERIODIC | LAPIC_LVT_MASK);
        lvt |= (IRQ_TIMER | TIMER_MODE_ONESHOT);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt);
        break;

    case CLOCK_EVDEV_STATE_PERIODIC:
        lvt &= ~LAPIC_LVT_MASK;
        lvt |= (IRQ_TIMER | TIMER_MODE_PERIODIC);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt);
        break;

    case CLOCK_EVDEV_STATE_OFF:
    case CLOCK_EVDEV_STATE_ONESHOT_STOPPED:
        lapic_write(LAPIC_REG_TIMER_INIT, 0);
        lvt |= LAPIC_LVT_MASK;
        lapic_write(LAPIC_REG_LVT_TIMER, lvt);
        break;
    }

    ced->state = state;
    return 0;
}

static struct clock_evdev *lapic_clock_evdev_create(cpu_id_t core_id) {
    static const uint64_t nanoseconds_per_khz_tick = 1000000;

    struct clock_evdev *ced = clock_evdev_create("lapic_timer_%zu", core_id);

    ced->set_next_event = lapic_evdev_set_next_event;
    ced->change_state = lapic_evdev_change_state;

    /* bounds based on LAPIC frequency */
    freq_khz_t freq_khz = global.cores[core_id]->lapic_khz;

    ced->min_delta_ns =
        (time_ns_t) DIV_ROUND_UP(nanoseconds_per_khz_tick, freq_khz);
    ced->max_delta_ns = (0xFFFFFFFFULL * 1000000ULL) / freq_khz;

    ced->min_delta_ticks = 1;
    ced->max_delta_ticks = 0xFFFFFFFF;

    ced->flags =
        CLOCK_EVDEV_ONESHOT | CLOCK_EVDEV_PERCPU | CLOCK_EVDEV_TICK_SUITABLE;
    ced->rating = CLOCK_RATING_BEST;
    ced->bound_to_cpu = core_id;
    ced->irq = IRQ_TIMER;

    /* ready LAPIC divider and vector */
    lapic_write(LAPIC_REG_TIMER_DIV, 0b0011);

    lapic_evdev_change_state(ced, CLOCK_EVDEV_STATE_ONESHOT_STOPPED);
    return ced;
}

void lapic_clock_evdev_group_init(void) {
    struct clock_evdev_group *cedg =
        nofail(clock_evdev_group_create(CLOCK_NAME_LAPIC));

    /* No need to set evdev_for_cpu if CLOCK_EVDEV_GROUP_PERCPU set */
    cedg->flags = CLOCK_EVDEV_GROUP_PERCPU;

    size_t i;
    for_each_cpu_id(i) {
        clock_evdev_group_add(cedg, lapic_clock_evdev_create(i));
    }

    clock_evdev_group_register(cedg);
}
