#include <console/crash.h>
#include <console/printf.h>
#include <stdarg.h>
#include <string.h>

static cc_noreturn void impl_default(struct crash_payload pluh,
                                     const char *file, int line,
                                     const char *func, const char *prefix,
                                     const char *fmt, va_list args) {
    cc_unused(pluh);
    static char msg[CRASH_MSG_MAX];

    int n = 0;
    if (prefix && strlen(prefix) > 0) {
        n = snprintf(msg, sizeof(msg), "%s", prefix);
        if (n < 0)
            n = 0;
        if (n > (int) sizeof(msg) - 1)
            n = (int) sizeof(msg) - 1;
    }

    if (fmt && strlen(fmt) > 0)
        vsnprintf(msg + n, sizeof(msg) - n, fmt, args);

    crash_full(&(struct crash_context){
        .payload = pluh,
        .source = CRASH_SOURCE_ENFORCE,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
    });
}

static cc_noreturn void impl_enforce_expr(struct crash_payload pluh,
                                          const char *file, int line,
                                          const char *func, const char *prefix,
                                          const char *expr,
                                          const char *fmt, va_list args) {
    cc_unused(pluh);
    static char msg[CRASH_MSG_MAX];

    int n = snprintf(msg, sizeof(msg), "%s%s", prefix ? prefix : "",
                     expr ? expr : "");

    if (n < 0)
        n = 0;
    if (n > (int) sizeof(msg) - 1)
        n = (int) sizeof(msg) - 1;

    if (fmt && strlen(fmt) > 0) {
        n += snprintf(msg + n, (int) sizeof(msg) - n, ": ");
        if (n > (int) sizeof(msg) - 1)
            n = (int) sizeof(msg) - 1;

        vsnprintf(msg + n, (int) sizeof(msg) - n, fmt, args);
    }

    crash_full(&(struct crash_context){
        .payload = pluh,
        .source = CRASH_SOURCE_ENFORCE,
        .formats = CRASH_FMT_DEFAULT,
        .file = file,
        .line = line,
        .func = func,
        .msg = msg,
    });
}

cc_noreturn void enforce_impl_default(struct crash_payload pluh,
                                      const char *file, int line,
                                      const char *func, const char *prefix,
                                      const char *expr, const char *fmt,
                                      ...) {
    va_list args;
    va_start(args, fmt);

    if (expr && strlen(expr) > 0) {
        impl_enforce_expr(pluh, file, line, func, prefix, expr, fmt, args);
    } else {
        impl_default(pluh, file, line, func, prefix, fmt, args);
    }

    va_end(args);
}
