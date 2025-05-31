#include <stdint.h>
#include <sleep.h>

extern uint64_t time_tick_count;

static void sleep_ticks(uint64_t ticks) {
    uint64_t start = time_tick_count;
    while ((time_tick_count - start) < ticks) {
        asm volatile("hlt");
    }
}

void sleep_ms(uint64_t ms) {
    sleep_ticks((ms * PIT_HZ) / 1000);
}

void sleep(uint64_t seconds) {
    sleep_ticks(seconds * PIT_HZ);
}
