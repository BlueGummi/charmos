#include <acpi/ioapic.h>
#include <asm.h>
#include <console/printf.h>
#include <irq/irq.h>
#include <log.h>
#include <pit.h>
#include <stdint.h>
#include <time/time.h>
#include <types/types.h>

LOG_HANDLE_DECLARE_DEFAULT(pit);

static inline void pit_write_command(uint8_t cmd) {
    outb(PIT_PORT_COMMAND, cmd);
}

static inline void pit_write_count16(uint8_t channel, uint16_t count) {
    uint16_t port = PIT_PORT_CHANNEL(channel);
    outb(port, (uint8_t) (count & 0xFF));
    outb(port, (uint8_t) ((count >> 8) & 0xFF));
}

uint16_t pit_read_counter(uint8_t channel) {
    pit_write_command(PIT_CMD_CHANNEL(channel) | PIT_CMD_ACCESS_LATCH);

    uint16_t port = PIT_PORT_CHANNEL(channel);
    uint8_t low = inb(port);
    uint8_t high = inb(port);

    return ((uint16_t) high << 8) | low;
}

static uint16_t pit_read_count(void) {
    return pit_read_counter(0);
}

static void pit_wait_until_zero(void) {
    while (true) {
        uint16_t count = pit_read_count();
        if (count <= 1)
            break;
        cpu_relax();
    }
}

void pit_set_divisor(uint16_t divisor) {
    pit_write_command(PIT_CMD_CHANNEL0 | PIT_CMD_ACCESS_LOHI |
                      PIT_CMD_MODE_RATE_GEN | PIT_CMD_BINARY);
    pit_write_count16(0, divisor);
}

void pit_set_periodic_mode(uint16_t divisor) {
    pit_set_divisor(divisor);
}

void pit_set_oneshot_mode(uint16_t divisor) {
    pit_write_command(PIT_CMD_CHANNEL0 | PIT_CMD_ACCESS_LOHI |
                      PIT_CMD_MODE_ONESHOT | PIT_CMD_BINARY);
    pit_write_count16(0, divisor);
}

void pit_set_frequency(freq_hz_t freq_hz) {
    if (freq_hz == 0)
        freq_hz = 1;

    uint32_t divisor;
    if (freq_hz >= PIT_BASE_FREQUENCY_HZ) {
        divisor = PIT_DIVISOR_MIN;
    } else {
        divisor = (uint32_t) (PIT_BASE_FREQUENCY_HZ / freq_hz);
        if (divisor > PIT_DIVISOR_MAX_COUNT)
            divisor = PIT_DIVISOR_ROLLOVER_VAL; /* 0 is 65536 in bin mode */
        else if (divisor < PIT_DIVISOR_MIN)
            divisor = PIT_DIVISOR_MIN;
    }

    pit_set_divisor((uint16_t) divisor);
}

void pit_set_interval_ns(time_ns_t interval_ns) {
    if (interval_ns == 0) {
        pit_set_divisor(PIT_DIVISOR_MIN);
        return;
    }

    uint64_t divisor = (interval_ns * PIT_BASE_FREQUENCY_HZ) / NS_PER_S;

    if (divisor > PIT_DIVISOR_MAX_COUNT)
        divisor = PIT_DIVISOR_ROLLOVER_VAL;

    else if (divisor < PIT_DIVISOR_MIN)
        divisor = PIT_DIVISOR_MIN;

    pit_set_divisor((uint16_t) divisor);
}

void pit_wire_irq(irq_t vector, irq_handler_t handler, void *ctx) {
    irq_register("pit_timer", vector, handler, ctx, IRQ_FLAG_NONE);
    irq_enable(vector);
}

void pit_wire_periodic_nmi(time_ns_t interval_ns) {
    pit_set_interval_ns(interval_ns);

    union ioapic_redirection_entry entry = {0};
    entry.vector = IRQ_NMI;
    entry.delivery_mode = IOAPIC_DELIVERY_NMI;

    /* PIT is IOAPIC IRQ 0 */

    /* TODO: ACPI MADT ISO IRQ 0 override if
     * PIT GSI goes to Pin 2 instead of 0 */
    ioapic_set_redirection_entry(0, entry.raw);
}

void pit_init(void) {
    pit_write_command(PIT_DEFAULT_MODE);
    pit_write_count16(0, PIT_DEFAULT_COUNT);
}

freq_hz_t pit_measure_tsc_freq(void) {
    pit_write_command(PIT_CALIBRATION_MODE);
    pit_write_count16(0, PIT_CALIBRATION_COUNT);

    uint64_t start_tsc = rdtsc();

    pit_wait_until_zero();

    uint64_t end_tsc = rdtsc();

    pit_write_command(PIT_DEFAULT_MODE);
    pit_write_count16(0, PIT_DEFAULT_COUNT);

    uint64_t tsc_frequency =
        (end_tsc - start_tsc) * PIT_FREQUENCY / PIT_CALIBRATION_COUNT;

    return tsc_frequency;
}
