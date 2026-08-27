/* @title: Kernel Panic Wrapper */
#include <console/crash.h>
#include <console/panic.h>
#include <console/printf.h>
#include <console/report.h>
#include <dbg.h>
#include <stdarg.h>
#include <string.h>

__noreturn void panic_impl_default(const char *file, int line, const char *func,
                                   const char *fmt, ...) {
    static char msg[REPORT_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    va_end(args);

    crash(&(struct crash_context){
        .source = CRASH_SOURCE_PANIC,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
        .regs = NULL,
    });
}

__noreturn void panic_impl_with_regs(const struct panic_regs *regs,
                                     const char *file, int line,
                                     const char *func, const char *fmt, ...) {
    static char msg[REPORT_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    va_end(args);

    crash(&(struct crash_context){
        .source = CRASH_SOURCE_PANIC,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
        .regs = regs,
    });
}
