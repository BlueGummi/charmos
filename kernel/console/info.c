#include <console/printf.h>
#include <dbg.h>

void k_info_impl(const char *category, int level, const char *file, int line,
                 const char *fmt, ...) {
    va_list args;

    k_printf("[%s%s" ANSI_RESET "]: ", k_log_level_color(level), category);

    /* Print message */
    va_start(args, fmt);
    k_vprintf(NULL, fmt, args);
    va_end(args);

    if (level != K_TEST)
        k_printf("\n");

    /* Extra info for WARN / ERROR */
    if (level == K_WARN || level == K_ERROR) {

        /* Source information */
        k_printf("    at %s:%d\n", file, line);

        /* Stack walk */
        debug_print_stack();
    }
}
