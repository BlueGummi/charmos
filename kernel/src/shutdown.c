#include <stddef.h>
#include <system/printf.h>
#include <stdint.h>

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %1, %0" ::"dN"(port), "a"(value));
}

// l for local
uint16_t l_SLP_EN = 1 << 13;
uint16_t l_sleep_command = 0;
uint16_t l_pm1b_ctrl = 0;
uint16_t l_pm1a_ctrl = 0;

void k_shutdown_init(uint16_t pm1a_control, uint16_t pm1b_control, uint16_t slp_typ) {
    l_sleep_command = slp_typ | l_SLP_EN;
    l_pm1a_ctrl = pm1a_control;
    l_pm1b_ctrl = pm1b_control;
}

void k_shutdown(void) {
    if (l_sleep_command == 0) {
        outw(0xB004, 0x2000);
        outw(0x604, 0x2000);
        return;
    }
    outw(l_pm1a_ctrl, l_sleep_command);

    if (l_pm1b_ctrl) {
        outw(l_pm1b_ctrl, l_sleep_command);
    }
}
