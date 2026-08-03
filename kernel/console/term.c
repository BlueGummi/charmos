/* @title: Console terminal state */
#include <asm.h>
#include <colors.h>
#include <console/printf.h>
#include <console/term.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

#define TERM_PROBE_SPIN_CAP (1u << 26)
#define TERM_ESC(s) serial_write((s), sizeof(s) - 1)

/* Properties of the console */
static struct {
    bool available;
    bool plain;
    bool unicode;
    bool panic;
    uint16_t rows;
    uint16_t cols;
} term = {.unicode = true};

bool term_available(void) {
    return term.available;
}

struct term_geometry term_size(void) {
    if (!term.available || !term.rows || !term.cols)
        return (struct term_geometry){.rows = TERM_FALLBACK_ROWS,
                                      .cols = TERM_FALLBACK_COLS};

    return (struct term_geometry){.rows = term.rows, .cols = term.cols};
}

void term_geometry_set(uint16_t rows, uint16_t cols) {
    if (rows < 2 || cols < 8)
        return;

    term.rows = rows;
    term.cols = cols;
    term.available = true;
}

void term_set_plain(bool plain) {
    term.plain = plain;
}

bool term_plain(void) {
    return term.plain;
}

void term_set_unicode(bool unicode) {
    term.unicode = unicode;
}

bool term_unicode(void) {
    return term.unicode;
}

bool term_in_panic(void) {
    return term.panic;
}

const char *term_style(enum term_sev sev) {
    if (term.plain)
        return "";

    switch (sev) {
    case TERM_SEV_DIM: return ANSI_GRAY;
    case TERM_SEV_LABEL: return ANSI_BRIGHT_BLUE;
    case TERM_SEV_OK: return ANSI_GREEN;
    case TERM_SEV_WARN: return ANSI_YELLOW;
    case TERM_SEV_CRIT: return ANSI_BOLD ANSI_RED;
    case TERM_SEV_HEAD: return ANSI_BOLD;
    case TERM_SEV_NORMAL:
    default: return "";
    }
}

const char *term_style_reset(void) {
    return term.plain ? "" : ANSI_RESET;
}

static void term_drain_rx(void) {
    char c;

    while (serial_try_getc(&c))
        ;
}

bool term_probe(void) {
    enum { WANT_ESC, WANT_CSI, WANT_ROWS, WANT_COLS } state = WANT_ESC;
    uint32_t rows = 0, cols = 0;
    uint64_t spins = 0;
    bool done = false;
    char c;

    if (term.panic)
        return term.available;

    term.available = false;
    term_drain_rx();

    /* Put the cursor past any reasonable bottom right corner, then ask where
     * it ended up, mildly hacky */
    TERM_ESC("\0337\033[999;999H\033[6n");

    time_t deadline = time_get_ms() + TERM_PROBE_TIMEOUT_MS;

    while (!done && time_get_ms() < deadline) {
        if (++spins > TERM_PROBE_SPIN_CAP)
            break;

        if (!serial_try_getc(&c)) {
            cpu_relax();
            continue;
        }

        switch (state) {
        case WANT_ESC:
            /* Whatever typed while waited is noise */
            if (c == '\033')
                state = WANT_CSI;
            break;
        case WANT_CSI:
            rows = cols = 0;
            state = (c == '[') ? WANT_ROWS : WANT_ESC;
            break;
        case WANT_ROWS:
            if (c >= '0' && c <= '9')
                rows = rows * 10 + (uint32_t) (c - '0');
            else if (c == ';')
                state = WANT_COLS;
            else
                state = WANT_ESC;
            break;
        case WANT_COLS:
            if (c >= '0' && c <= '9')
                cols = cols * 10 + (uint32_t) (c - '0');
            else if (c == 'R')
                done = true;
            else
                state = WANT_ESC;
            break;
        }
    }

    TERM_ESC("\0338");

    /* We need at least two rows to scroll and hold the bar */
    if (!done || rows < 2 || rows > UINT16_MAX || cols < 8 || cols > UINT16_MAX)
        return false;

    term.rows = (uint16_t) rows;
    term.cols = (uint16_t) cols;
    term.available = true;
    return true;
}

void term_enter_panic(void) {
    term.panic = true;

    if (term.available)
        TERM_ESC("\033[r\033[?25h");
}
