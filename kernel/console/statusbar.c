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
#include <sync/raw_spinlock.h>
#include <time/time.h>
#include <time/timer.h>

#define BAR_MIN_COLS 10
#define BAR_MAX_COLS 40

#define BAR_ESC(s) serial_write((s), sizeof(s) - 1)

struct bar_progress {
    size_t done;
    size_t total;
    time_ms_t test_started_ms;
    time_ms_t total_started_ms;
    bool timed;
    bool active;
    char detail[REPORT_LINE_MAX];
    char metadata_storage[REPORT_LINE_MAX];
    char progress_storage[REPORT_LINE_MAX];
};

static struct raw_spinlock bar_lock = RAW_SPINLOCK_INIT;
static bool bar_open;
static struct bar_progress bar_progress;

static void bar_timer_fn(struct timer *timer);
TIMER_DECLARE(bar_timer, bar_timer_fn);

static void bar_timer_arm(void) {
    timer_modify(&bar_timer, timer_delta_us(MS_TO_US(1000)));
}

void status_bar_open(void) {
    if (!term_available() || term_in_panic())
        return;

    if (term_size().rows < 3)
        return;

    bool irqs_were_enabled = raw_spin_lock_irq_disable(&bar_lock);
    if (bar_open) {
        raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);
        return;
    }
    bar_open = true;
    raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);

    enum irql irql = printf_lock();

    BAR_ESC("\r\n\r\n\r\n");

    char buf[64];
    int n = snprintf(buf, (int) sizeof(buf), "\033[1;%ur\033[%u;1H",
                     (uint32_t) (term_size().rows - 2),
                     (uint32_t) (term_size().rows - 2));
    if (n > 0)
        serial_write(buf, (size_t) n);

    printf_unlock(irql);

    timer_init(&bar_timer, bar_timer_fn, NULL);
    bar_timer_arm();
}

void status_bar_close(void) {
    if (!term_available())
        return;

    bool irqs_were_enabled = raw_spin_lock_irq_disable(&bar_lock);
    if (!bar_open) {
        raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);
        return;
    }
    bar_open = false;
    bar_progress.active = false;
    raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);

    timer_shutdown_sync(&bar_timer);

    enum irql irql = printf_lock();

    char buf[64];
    int n =
        snprintf(buf, (int) sizeof(buf),
                 "\033[%u;1H\033[2K\033[%u;1H\033[2K\033[r\033[%u;1H",
                 (uint32_t) (term_size().rows - 1), (uint32_t) term_size().rows,
                 (uint32_t) /* avoid extra newline */ term_size().rows - 2);
    if (n > 0)
        serial_write(buf, (size_t) n);

    printf_unlock(irql);
}

static void bar_begin(void) {
    char buf[64];
    int n = snprintf(buf, (int) sizeof(buf), "\0337\033[?25l\033[%u;1H\033[2K",
                     (uint32_t) (term_size().rows - 1));
    if (n > 0)
        serial_write(buf, (size_t) n);
}

static void bar_end(const struct report_line *metadata,
                    const struct report_line *progress) {
    serial_write(report_line_str(metadata), metadata->len);
    char buf[32];
    int n = snprintf(buf, (int) sizeof(buf), "\033[%u;1H\033[2K",
                     (uint32_t) term_size().rows);
    if (n > 0)
        serial_write(buf, (size_t) n);
    serial_write(report_line_str(progress), progress->len);
    BAR_ESC(ANSI_RESET "\033[?25h\0338");
}

void status_bar_set(const char *fmt, ...) {
    va_list ap;

    if (!term_available() || term_in_panic())
        return;

    REPORT_LINE(l, term_size().cols);

    va_start(ap, fmt);
    report_line_vprintf(&l, fmt, ap);
    va_end(ap);

    enum irql irql = printf_lock();
    bool irqs_were_enabled = raw_spin_lock_irq_disable(&bar_lock);
    bool open = bar_open;
    raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);
    if (open) {
        bar_begin();
        REPORT_LINE(empty, term_size().cols);
        bar_end(&empty, &l);
    }
    printf_unlock(irql);
}

static void bar_format_duration(time_ms_t elapsed, char *buf, size_t cap) {
    uint64_t seconds = MS_TO_SECONDS(elapsed);
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    minutes %= 60;
    seconds %= 60;

    if (hours) {
        snprintf(buf, (int) cap, "%lluh %llum %llus",
                 (unsigned long long) hours, (unsigned long long) minutes,
                 (unsigned long long) seconds);
    } else if (minutes) {
        snprintf(buf, (int) cap, "%llum %llus", (unsigned long long) minutes,
                 (unsigned long long) seconds);
    } else {
        snprintf(buf, (int) cap, "%llus", (unsigned long long) seconds);
    }
}

static void bar_progress_render(struct bar_progress *progress) {
    size_t done = progress->done;
    size_t total = progress->total;

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

    struct report_line metadata;
    struct report_line progress_line;
    report_line_init(&metadata, progress->metadata_storage,
                     sizeof(progress->metadata_storage), term_size().cols);
    report_line_init(&progress_line, progress->progress_storage,
                     sizeof(progress->progress_storage), term_size().cols);

    report_line_puts(&progress_line, ANSI_BOLD ANSI_BLUE);
    report_line_puts(&progress_line, cap_l);
    report_line_puts(&progress_line, ANSI_GREEN);
    report_line_repeat(&progress_line, on, filled);

    report_line_puts(&progress_line, ANSI_GRAY);
    report_line_repeat(&progress_line, off, width - filled);

    report_line_puts(&progress_line, ANSI_BLUE);
    report_line_puts(&progress_line, cap_r);
    report_line_puts(&progress_line, ANSI_RESET);

    char formatted[128];
    snprintf(formatted, (int) sizeof(formatted),
             " " ANSI_BOLD "%zu" ANSI_RESET "/%zu " ANSI_BOLD "%zu%%" ANSI_RESET
             "  ",
             done, total, pct);
    report_line_puts(&progress_line, formatted);

    report_line_puts(&metadata, progress->detail);

    if (progress->timed) {
        time_ms_t now = time_get_ms();
        time_ms_t total_elapsed = now - progress->total_started_ms;

        report_line_puts(&metadata, "  " ANSI_GRAY);
        if (progress->test_started_ms) {
            time_ms_t test_elapsed = now - progress->test_started_ms;
            bar_format_duration(test_elapsed, formatted, sizeof(formatted));
            report_line_puts(&metadata, formatted);
            report_line_puts(&metadata, " (total elapsed: ");
        } else {
            report_line_puts(&metadata, "total elapsed: ");
        }
        bar_format_duration(total_elapsed, formatted, sizeof(formatted));
        report_line_puts(&metadata, formatted);
        if (progress->test_started_ms)
            report_line_puts(&metadata, ")");
        report_line_puts(&metadata, ANSI_RESET);
    }

    bar_begin();
    bar_end(&metadata, &progress_line);
}

static void status_bar_progress_v(size_t done, size_t total,
                                  time_ms_t test_started_ms,
                                  time_ms_t total_started_ms,
                                  const char *detail_fmt, va_list ap) {
    if (!term_available() || term_in_panic())
        return;

    enum irql irql = printf_lock();
    bool irqs_were_enabled = raw_spin_lock_irq_disable(&bar_lock);
    if (!bar_open) {
        raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);
        printf_unlock(irql);
        return;
    }
    bar_progress.done = done;
    bar_progress.total = total;
    bar_progress.test_started_ms = test_started_ms;
    bar_progress.total_started_ms = total_started_ms;
    bar_progress.timed = total_started_ms != 0;
    bar_progress.active = true;
    vsnprintf(bar_progress.detail, (int) sizeof(bar_progress.detail),
              detail_fmt, ap);
    raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);
    bar_progress_render(&bar_progress);
    printf_unlock(irql);
}

void status_bar_progress(size_t done, size_t total, const char *detail_fmt,
                         ...) {
    va_list ap;

    va_start(ap, detail_fmt);
    status_bar_progress_v(done, total, 0, 0, detail_fmt, ap);
    va_end(ap);
}

void status_bar_progress_timed(size_t done, size_t total,
                               time_ms_t test_started_ms,
                               time_ms_t total_started_ms,
                               const char *detail_fmt, ...) {
    va_list ap;

    va_start(ap, detail_fmt);
    status_bar_progress_v(done, total, test_started_ms, total_started_ms,
                          detail_fmt, ap);
    va_end(ap);
}

static void bar_timer_fn(struct timer *timer) {
    (void) timer;

    if (!term_available() || term_in_panic())
        return;

    enum irql irql = printf_lock();
    bool irqs_were_enabled = raw_spin_lock_irq_disable(&bar_lock);
    bool repaint = bar_open && bar_progress.active && bar_progress.timed;
    bool open = bar_open;
    raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);
    if (repaint) {
        bar_progress_render(&bar_progress);
    }
    if (open)
        bar_timer_arm();
    printf_unlock(irql);
}

void status_bar_reset(void) {
    if (!term_available())
        return;

    bool irqs_were_enabled = raw_spin_lock_irq_disable(&bar_lock);
    bar_open = false;
    bar_progress.active = false;
    raw_spin_unlock_irq_restore(&bar_lock, irqs_were_enabled);

    char buf[64];
    int n = snprintf(buf, (int) sizeof(buf), "\033[r\033[?25h\033[%u;1H\r\n",
                     (uint32_t) term_size().rows);
    if (n > 0)
        serial_write(buf, (size_t) n);
}
