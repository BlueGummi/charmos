#include <asm.h>
#include <ndjson.h>
#include <stdatomic.h>
#include <sync/raw_spinlock.h>

#include "internal.h"

#define UART_DATA 0
#define UART_INT_ENABLE 1
#define UART_FIFO_CTRL 2
#define UART_LINE_CTRL 3
#define UART_MODEM_CTRL 4
#define UART_LINE_STATUS 5

#define UART_LSR_THRE 0x20 /* transmit holding register empty */
#define UART_LCR_DLAB 0x80
#define UART_LCR_8N1 0x03
#define UART_DIVISOR 0x03 /* 38400 baud */

#define NDJSON_PANIC_LOCK_SPINS 1000000

/* Panic transport must remain usable when normal lock validation fails. */
static struct raw_spinlock carrier_lock = RAW_SPINLOCK_INIT;
static uint16_t carrier_port;
static atomic_bool carrier_up;
static atomic_bool carrier_panicked;

void ndjson_carrier_init(uint16_t port) {
    outb(port + UART_INT_ENABLE, 0x00);
    outb(port + UART_LINE_CTRL, UART_LCR_DLAB);
    outb(port + UART_DATA, UART_DIVISOR);
    outb(port + UART_INT_ENABLE, 0x00);
    outb(port + UART_LINE_CTRL, UART_LCR_8N1);
    outb(port + UART_FIFO_CTRL, 0xC7);
    outb(port + UART_MODEM_CTRL, 0x0B);

    carrier_port = port;
    atomic_store_explicit(&carrier_up, true, memory_order_release);
}

void ndjson_carrier_disable(void) {
    atomic_store_explicit(&carrier_up, false, memory_order_release);
}

bool ndjson_carrier_online(void) {
    return atomic_load_explicit(&carrier_up, memory_order_acquire);
}

void ndjson_enter_panic(void) {
    atomic_store_explicit(&carrier_panicked, true, memory_order_release);
    raw_spin_unlock(&carrier_lock);
}

/* Raw spinlocks here, because if something IRQL related panics,
 * it would not be able to use ndjson to log */
bool ndjson_carrier_begin(bool *irqs_were_on) {
    *irqs_were_on = false;

    if (!ndjson_carrier_online())
        return false;

    *irqs_were_on = are_interrupts_enabled();
    disable_interrupts();

    if (atomic_load_explicit(&carrier_panicked, memory_order_acquire)) {
        for (uint64_t i = 0; i < NDJSON_PANIC_LOCK_SPINS; i++) {
            if (raw_spin_trylock(&carrier_lock))
                break;
            cpu_relax();
        }
        return true;
    }

    raw_spin_lock(&carrier_lock);
    return true;
}

void ndjson_carrier_end(bool irqs_were_on) {
    raw_spin_unlock(&carrier_lock);

    if (irqs_were_on)
        enable_interrupts();
}

void ndjson_carrier_putc(char c) {
    while (!(inb(carrier_port + UART_LINE_STATUS) & UART_LSR_THRE))
        cpu_relax();

    outb(carrier_port + UART_DATA, (uint8_t) c);
}

void ndjson_carrier_write(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        ndjson_carrier_putc(s[i]);
}
