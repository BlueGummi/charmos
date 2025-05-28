#include <stdint.h>

#define PIT_FREQUENCY 1193182

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

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
    outb(0x40, (uint8_t)(pit_count & 0xFF));
    outb(0x40, (uint8_t)(pit_count >> 8));

    uint64_t start_tsc = rdtsc();

    pit_wait_until_zero();

    uint64_t end_tsc = rdtsc();

    uint64_t tsc_frequency = (end_tsc - start_tsc) * PIT_FREQUENCY / pit_count;

    return tsc_frequency;
}

