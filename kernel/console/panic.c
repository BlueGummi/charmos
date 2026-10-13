#include <console/crash.h>
#include <console/panic.h>
#include <console/printf.h>
#include <console/report.h>
#include <dbg.h>
#include <stdarg.h>
#include <string.h>
#include <sync/raw_spinlock.h>

static char msg[REPORT_LINE_MAX];
static struct raw_spinlock panic_msg_lock = RAW_SPINLOCK_INIT;

static cc_noreturn void panic_crash(enum crash_code code, const char *file,
                                    int line, const char *func) {
    crash_full(&(struct crash_context){
        .payload = CRASH_CODE_TO_PAYLOAD(code),
        .source = CRASH_SOURCE_PANIC,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
        .regs = NULL,
    });
}

static cc_noreturn void panic_v(enum crash_code code, const char *file,
                                int line, const char *func, const char *fmt,
                                va_list args) {
    raw_spin_lock(&panic_msg_lock);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    panic_crash(code, file, line, func);
}

cc_noreturn void panic_impl_msg(const char *file, int line, const char *func,
                                const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    panic_v(CRASH_CODE_GENERIC, file, line, func, fmt, args);
    va_end(args);
}

cc_noreturn void panic_impl_code(const char *file, int line, const char *func,
                                 enum crash_code code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    panic_v(code, file, line, func, fmt, args);
    va_end(args);
}

cc_noreturn void panic_impl_code_only(const char *file, int line,
                                      const char *func, enum crash_code code) {
    raw_spin_lock(&panic_msg_lock);
    snprintf(msg, (int) sizeof(msg), "No message supplied");
    panic_crash(code, file, line, func);
}
