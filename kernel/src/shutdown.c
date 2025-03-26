#include <stdint.h>
#include <system/printf.h>

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %1, %0" ::"dN"(port), "a"(value));
}

void k_shutdown(void) {
    outw(0x604, 0x2000);
}
