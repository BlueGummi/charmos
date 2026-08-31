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
__noreturn void panic_impl_default(struct crash_payload pluh, const char *file,
                                   int line, const char *func, const char *fmt,
                                   ...) {
    unused(pluh);
    raw_spin_lock(&panic_msg_lock);
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    va_end(args);

    crash_full(&(struct crash_context){
        .source = CRASH_SOURCE_PANIC,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
        .regs = NULL,
    });
}
