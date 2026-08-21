/* @title: Programmable Interval Timer */
#pragma once
#include <irq/irq.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time/time.h>
#include <types/types.h>

/* IO ports */
#define PIT_PORT_CHANNEL0 0x40
#define PIT_PORT_CHANNEL1 0x41
#define PIT_PORT_CHANNEL2 0x42
#define PIT_PORT_COMMAND 0x43

#define PIT_PORT_CHANNEL(ch) ((uint16_t) (PIT_PORT_CHANNEL0 + (ch)))

/* PIT Base Frequency (1.193182 MHz) */
#define PIT_BASE_FREQUENCY_HZ 1193182ULL
#define PIT_FREQUENCY 1193182ULL

/*
 * PIT Command Register (PIT_PORT_COMMAND) Bit Definitions
 *
 *  Bit 7..6: Channel Selection
 *  Bit 5..4: Access / Operating Mode
 *  Bit 3..1: Operating Mode (0-5)
 *  Bit 0:    BCD / Binary (0 = 16 bit binary, 1 = BCD)
 */

#define PIT_CMD_CHANNEL(ch) ((uint8_t) (((ch) & 0x3) << 6))
#define PIT_CMD_CHANNEL0 0x00
#define PIT_CMD_CHANNEL1 0x40
#define PIT_CMD_CHANNEL2 0x80
#define PIT_CMD_READBACK 0xC0

#define PIT_CMD_ACCESS_LATCH 0x00 /* Latch count value command */
#define PIT_CMD_ACCESS_LO 0x10    /* Low byte only */
#define PIT_CMD_ACCESS_HI 0x20    /* High byte only */
#define PIT_CMD_ACCESS_LOHI 0x30  /* Low byte then high byte (16 bit) */

/* Operating Modes */
#define PIT_CMD_MODE_ONESHOT 0x00
#define PIT_CMD_MODE_HW_ONESHOT 0x02
#define PIT_CMD_MODE_RATE_GEN 0x04
#define PIT_CMD_MODE_SQUARE_WAVE 0x06
#define PIT_CMD_MODE_SW_STROBE 0x08
#define PIT_CMD_MODE_HW_STROBE 0x0A

/* Binary / BCD */
#define PIT_CMD_BINARY 0x00
#define PIT_CMD_BCD 0x01

#define PIT_DEFAULT_MODE 0x34
#define PIT_DEFAULT_COUNT 0x4AF2
#define PIT_CALIBRATION_MODE 0x30
#define PIT_CALIBRATION_COUNT UINT16_MAX

#define PIT_DIVISOR_MIN 1
#define PIT_DIVISOR_MAX_COUNT UINT16_MAX
#define PIT_DIVISOR_ROLLOVER_VAL 0 /* 0 is 65536 in binary mode */

void pit_init(void);

void pit_set_divisor(uint16_t divisor);
void pit_set_frequency(freq_hz_t freq_hz);
void pit_set_interval_ns(time_ns_t interval_ns);
void pit_set_periodic_mode(uint16_t divisor);
void pit_set_oneshot_mode(uint16_t divisor);
uint16_t pit_read_counter(uint8_t channel);
void pit_wire_irq(irq_t vector, irq_handler_t handler, void *ctx);
void pit_wire_periodic_nmi(time_ns_t interval_ns);
freq_hz_t pit_measure_tsc_freq(void);

static inline void pit_set_interval_us(time_us_t interval_us) {
    pit_set_interval_ns(interval_us * NS_PER_US);
}

static inline void pit_set_interval_ms(time_ms_t interval_ms) {
    pit_set_interval_ns(interval_ms * NS_PER_MS);
}
