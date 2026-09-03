#include <console/crash.h>
#include <console/printf.h>
#include <stdarg.h>
#include <string.h>

__noreturn void assert_impl_default(struct crash_payload pluh, const char *file,
                                    int line, const char *func, const char *fmt,
                                    ...) {
    unused(pluh);
    static char msg[CRASH_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    crash_full(&(struct crash_context){
        .payload = pluh,
        .source = CRASH_SOURCE_ASSERT,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
    });
}

__noreturn void assert_impl_assertion(struct crash_payload pluh,
                                      const char *file, int line,
                                      const char *func, const char *prefix,
                                      const char *assertion, const char *fmt,
                                      ...) {
    unused(pluh);
    static char msg[CRASH_MSG_MAX];

    int n = snprintf(msg, sizeof(msg), "%s%s", prefix ? prefix : "",
                     assertion ? assertion : "");

    if (n < 0)
        n = 0;
    if (n > (int) sizeof(msg) - 1)
        n = (int) sizeof(msg) - 1;

    if (fmt) {
        n += snprintf(msg + n, (int) sizeof(msg) - n, ": ");
        if (n > (int) sizeof(msg) - 1)
            n = (int) sizeof(msg) - 1;

        va_list args;
        va_start(args, fmt);
        vsnprintf(msg + n, (int) sizeof(msg) - n, fmt, args);
        va_end(args);
    }

    crash_full(&(struct crash_context){
        .payload = pluh,
        .source = CRASH_SOURCE_ASSERT,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
    });
}
