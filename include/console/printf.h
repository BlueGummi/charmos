#pragma once
#include <asm.h>
#include <charmos.h>
#include <flanterm/src/flanterm.h>
#include <flanterm/src/flanterm_backends/fb.h>
#include <limine.h>
#include <misc/colors.h>
#include <misc/logo.h>
#include <sync/spinlock.h>
#include <time.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)
#define LINE_STRING STRINGIZE(__LINE__)

void debug_print_stack();
extern void panic_entry();

#define ELEVEN_LINES "==========="
#define TWENTY_TWO_LINES ELEVEN_LINES ELEVEN_LINES
#define FORTY_FOUR_LINES TWENTY_TWO_LINES TWENTY_TWO_LINES
#define EIGHTY_EIGHT_LINES FORTY_FOUR_LINES FORTY_FOUR_LINES
extern struct spinlock panic_lock;

enum k_log_level {
    K_INFO,
    K_WARN,
    K_ERROR,
    K_TEST,
};

static inline const char *k_log_level_color(enum k_log_level l) {
    switch (l) {
    case K_INFO: return ANSI_GREEN;
    case K_WARN: return ANSI_YELLOW;
    case K_ERROR: return ANSI_RED;
    case K_TEST: return ANSI_BLUE;
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
        if (level != K_TEST)                                                   \
            k_printf("\n");                                                    \
    } while (0)

void k_printf(const char *format, ...);
void panic(const char *format, ...);
void serial_init();
void k_printf_init(struct limine_framebuffer *fb);
#pragma once
