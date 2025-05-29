#include <io.h>
#include <pit.h>
#include <stdint.h>

static uint16_t pit_read_count() {
    outb(0x43, 0x00);

    uint8_t low = inb(0x40);
    uint8_t high = inb(0x40);

    return ((uint16_t) high << 8) | low;
}

static void pit_wait_until_zero() {
    while (1) {
        uint16_t count = pit_read_count();
        if (count <= 1)
            break;
        __asm__ volatile("pause");
    }
}

uint64_t measure_tsc_freq_pit(void) {
    outb(0x43, 0x30);

    uint16_t pit_count = 0xFFFF;
    outb(0x40, (uint8_t) (pit_count & 0xFF));
    outb(0x40, (uint8_t) (pit_count >> 8));

    uint64_t start_tsc = rdtsc();

    pit_wait_until_zero();

    uint64_t end_tsc = rdtsc();

    uint64_t tsc_frequency = (end_tsc - start_tsc) * PIT_FREQUENCY / pit_count;

    return tsc_frequency;
}
