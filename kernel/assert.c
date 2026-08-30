#include <console/crash.h>
#include <console/printf.h>
#include <stdarg.h>
#include <string.h>

void __assert_fail(const char *assertion, const char *file, unsigned int line,
                   const char *function) {
    char msg[CRASH_MSG_MAX];
    snprintf(msg, sizeof(msg), "Assertion \"%s\" failed",
             assertion ? assertion : "<unknown>");
    crash_full(&(struct crash_context){
        .source = CRASH_SOURCE_ASSERT,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = (int) line,
        .func = function,
        .msg = msg,
    });
}

__noreturn void assert_impl_default(const char *file, int line,
                                    const char *func, const char *fmt, ...) {
    static char msg[CRASH_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    crash_full(&(struct crash_context){
        .source = CRASH_SOURCE_ASSERT,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
    });
}
