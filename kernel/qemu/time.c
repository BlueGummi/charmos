#include <io.h>
#include <printf.h>
#include <stdbool.h>
#include <stdint.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71

uint8_t read_cmos(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

bool rtc_is_updating() {
    outb(CMOS_ADDRESS, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd / 16) * 10);
}

void get_time(uint8_t *hour, uint8_t *minute, uint8_t *second) {
    while (rtc_is_updating())
        ;

    *second = read_cmos(0x00);
    *minute = read_cmos(0x02);
    *hour = read_cmos(0x04);

    uint8_t status_b = read_cmos(0x0B);
    if (!(status_b & 0x04)) {
        *second = bcd_to_bin(*second);
        *minute = bcd_to_bin(*minute);
        *hour = bcd_to_bin(*hour);
    }
}

void print_current_time() {
    uint8_t hour, minute, second;
    get_time(&hour, &minute, &second);

    k_printf("%02d:%02d:%02d", hour, minute, second);
}
