#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <limine.h>
#include <misc/colors.h>
#include <time/time.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)
#define LINE_STRING STRINGIZE(__LINE__)
#define k_panic(fmt, ...)                                                      \
    do {                                                                       \
        k_printf(                                                              \
            "\n+-\033[31m!!!\033[0m[\033[91mKERNEL PANIC\033[0m]\033[31m!!!"   \
            "\033[0m\n|\n+-> [\033[92mFROM\033[0m] \033[32m" __FILE__          \
            "\033[0m:\033[32m" LINE_STRING                                     \
            "\033[0m\n+-> [\033[33mMESSAGE\033[0m] "                           \
            "\033[31m");                                                       \
        panic(fmt, ##__VA_ARGS__);                                             \
        k_printf("\033[0m");                                                   \
    } while (0)

#define k_info(fmt, ...)                                                       \
    do {                                                                       \
        k_printf("[");                                                         \
        k_printf(ANSI_GREEN);                                                  \
        time_print_current();                                                  \
        k_printf(ANSI_RESET);                                                  \
        k_printf("]");                                                         \
        k_printf(": [%sINFO%s]: ", ANSI_YELLOW, ANSI_RESET);                   \
        k_printf(fmt, ##__VA_ARGS__);                                          \
        k_printf("\n");                                                        \
    } while (0)

#define k_warn(fmt, ...)                                                       \
    do {                                                                       \
        k_printf("[");                                                         \
        k_printf(ANSI_GREEN);                                                  \
        time_print_current();                                                  \
        k_printf(ANSI_RESET);                                                  \
        k_printf("]");                                                         \
        k_printf(": [%sWARN%s]: ", ANSI_BRIGHT_RED, ANSI_RESET);               \
        k_printf(fmt, ##__VA_ARGS__);                                          \
        k_printf("\n");                                                        \
    } while (0)

void k_printf(const char *format, ...);
void panic(const char *format, ...);
void serial_init();
void k_printf_init(struct limine_framebuffer *fb);
#pragma once
