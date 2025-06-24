#include <acpi/hpet.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <limine.h>
#include <misc/colors.h>
#include <misc/logo.h>
#include <time/time.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)
#define LINE_STRING STRINGIZE(__LINE__)

void debug_print_stack();

#define ELEVEN_LINES "==========="
#define TWENTY_TWO_LINES ELEVEN_LINES ELEVEN_LINES
#define FORTY_FOUR_LINES TWENTY_TWO_LINES TWENTY_TWO_LINES
#define EIGHTY_EIGHT_LINES FORTY_FOUR_LINES FORTY_FOUR_LINES

#define k_panic(fmt, ...)                                                      \
    do {                                                                       \
        asm("cli");                                                            \
        k_printf("\n" EIGHTY_EIGHT_LINES "\n");                                \
        k_printf("\n                                    [" ANSI_BG_RED         \
                 "KERNEL PANIC" ANSI_RESET "]\n\n");                           \
        k_printf(ANSI_RED "%s\n\n" ANSI_RESET, OS_LOGO_PANIC_CENTERED);        \
        k_printf("    [" ANSI_BRIGHT_BLUE "AT" ANSI_RESET " ");                \
        time_print_current();                                                  \
        k_printf("]\n");                                                       \
        k_printf("    [" ANSI_BRIGHT_GREEN "FROM" ANSI_RESET                   \
                 "] " ANSI_GREEN __FILE__ ANSI_RESET                           \
                 ":" ANSI_GREEN LINE_STRING ANSI_RESET ":" ANSI_CYAN           \
                 "%s()" ANSI_RESET "\n"                                        \
                 "    [" ANSI_YELLOW "MESSAGE" ANSI_RESET "] ",                \
                 __func__);                                                    \
        k_printf(fmt, ##__VA_ARGS__);                                          \
        debug_print_stack();                                                   \
        k_printf("\n" EIGHTY_EIGHT_LINES "\n");                                \
        while (1)                                                              \
            asm("hlt");                                                        \
    } while (0)

enum k_log_level {
    K_INFO,
    K_WARN,
    K_ERROR,
};

static inline const char *k_log_level_color(enum k_log_level l) {
    switch (l) {
    case K_INFO: return ANSI_GREEN;
    case K_WARN: return ANSI_YELLOW;
    case K_ERROR: return ANSI_RED;
    default: return ANSI_RESET;
    }
}

#define print_ms(ms)                                                           \
    do {                                                                       \
        int seconds = ms / 1000;                                               \
        int milliseconds = ms % 1000;                                          \
        k_printf("%d.%03d", seconds, milliseconds);                            \
    } while (0)

#define k_info(category, level, fmt, ...)                                      \
    do {                                                                       \
        k_printf("[");                                                         \
        k_printf(k_log_level_color(level));                                    \
        k_printf("%s", category);                                              \
        k_printf(ANSI_RESET);                                                  \
        k_printf("]: ");                                                       \
        k_printf(fmt, ##__VA_ARGS__);                                          \
        k_printf("\n");                                                        \
    } while (0)

void k_printf(const char *format, ...);
void panic(const char *format, ...);
void serial_init();
void k_printf_init(struct limine_framebuffer *fb);
#pragma once
