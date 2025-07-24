#include <asm.h>
#include <sch/defer.h>
#include <sch/sched.h>

static void timer_wakeup(void *data, void *unused) {
    (void) unused;
    struct idle_thread_data *idle = data;
}

static inline void setup_idle_timer(struct idle_thread_data *idle, time_t ms) {
    defer_enqueue(timer_wakeup, idle, NULL, ms);
}

static inline void do_idle_fast_hlt(struct idle_thread_data *idle) {
    setup_idle_timer(idle, IDLE_THREAD_CHECK_MS);
    enable_interrupts();
    wait_for_interrupt();
}

static void do_idle_loop(struct idle_thread_data *idle,
                         struct scheduler *sched) {
    switch (idle->state) {
    case IDLE_THREAD_FAST_HLT: do_idle_fast_hlt(idle); break;
    }
}

void scheduler_idle_main(void) {
    while(1){enable_interrupts();wait_for_interrupt();}

    struct idle_thread_data *idle = get_this_core_idle_thread();
    idle->last_entry_ms = time_get_ms();
    idle->state = IDLE_THREAD_FAST_HLT;

    struct scheduler *sched = get_this_core_sched();
    scheduler_disable_timeslice();
    enable_interrupts();

    while (true) {
        do_idle_loop(idle, sched);
    }
}
