/* @title: Pinned status bar */
#include <asm.h>
#include <colors.h>
#include <console/printf.h>
#include <console/report.h>
#include <console/statusbar.h>
#include <console/term.h>
#include <sch/irql.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BAR_MIN_COLS 10
#define BAR_MAX_COLS 40

#define BAR_ESC(s) serial_write((s), sizeof(s) - 1)

static bool bar_open;

void status_bar_open(void) {
    if (!term_available() || bar_open || term_in_panic())
        return;

    enum irql irql = printf_lock();

    BAR_ESC("\r\n\r\n");

    char buf[64];
    int n = snprintf(buf, (int) sizeof(buf), "\033[1;%ur\033[%u;1H",
                     (uint32_t) (term_size().rows - 1),
                     (uint32_t) (term_size().rows - 1));
    if (n > 0)
        serial_write(buf, (size_t) n);

    bar_open = true;
    printf_unlock(irql);
}

void status_bar_close(void) {
    if (!term_available() || !bar_open)
        return;

    enum irql irql = printf_lock();

    char buf[64];
    int n =
        snprintf(buf, (int) sizeof(buf), "\033[%u;1H\033[2K\033[r\033[%u;1H",
                 (uint32_t) term_size().rows, (uint32_t) term_size().rows);
    if (n > 0)
        serial_write(buf, (size_t) n);

    bar_open = false;
    printf_unlock(irql);
}

static void bar_begin(void) {
    char buf[64];
    int n = snprintf(buf, (int) sizeof(buf), "\0337\033[?25l\033[%u;1H\033[2K",
                     (uint32_t) term_size().rows);
    if (n > 0)
        serial_write(buf, (size_t) n);
}

static void bar_end(const struct report_line *l) {
    serial_write(report_line_str(l), l->len);
    BAR_ESC(ANSI_RESET "\033[?25h\0338");
}

void status_bar_set(const char *fmt, ...) {
    va_list ap;

    if (!term_available() || !bar_open || term_in_panic())
        return;

    REPORT_LINE(l, term_size().cols);

    va_start(ap, fmt);
    report_line_vprintf(&l, fmt, ap);
    va_end(ap);

    enum irql irql = printf_lock();
    bar_begin();
    bar_end(&l);
    printf_unlock(irql);
}

void status_bar_progress(size_t done, size_t total, const char *detail_fmt,
                         ...) {
    va_list ap;

    if (!term_available() || !bar_open || term_in_panic())
        return;

    if (done > total)
        done = total;

    size_t width = term_size().cols / 3;
    if (width < BAR_MIN_COLS)
        width = BAR_MIN_COLS;
    if (width > BAR_MAX_COLS)
        width = BAR_MAX_COLS;

    size_t filled = total ? (width * done) / total : width;
    size_t pct = total ? (100 * done) / total : 100;

    bool uni = term_unicode() && !term_plain();
    const char *cap_l = uni ? "▐" : "[";
    const char *cap_r = uni ? "▌" : "]";
    const char *on = uni ? "█" : "#";
    const char *off = uni ? "░" : "-";

    REPORT_LINE(l, term_size().cols);

    report_line_puts(&l, ANSI_BOLD ANSI_BLUE);
    report_line_puts(&l, cap_l);
    report_line_puts(&l, ANSI_GREEN);
    report_line_repeat(&l, on, filled);

    report_line_puts(&l, ANSI_GRAY);
    report_line_repeat(&l, off, width - filled);

    report_line_puts(&l, ANSI_BLUE);
    report_line_puts(&l, cap_r);
    report_line_puts(&l, ANSI_RESET);

    report_line_printf(&l,
                       " " ANSI_BOLD "%zu" ANSI_RESET "/%zu " ANSI_BOLD
                       "%zu%%" ANSI_RESET "  ",
                       done, total, pct);

    va_start(ap, detail_fmt);
    report_line_vprintf(&l, detail_fmt, ap);
    va_end(ap);

    enum irql irql = printf_lock();
    bar_begin();
    bar_end(&l);
    printf_unlock(irql);
}

void status_bar_reset(void) {
    if (!term_available())
        return;

    bar_open = false;

    char buf[64];
    int n = snprintf(buf, (int) sizeof(buf), "\033[r\033[?25h\033[%u;1H\r\n",
                     (uint32_t) term_size().rows);
    if (n > 0)
        serial_write(buf, (size_t) n);
}
