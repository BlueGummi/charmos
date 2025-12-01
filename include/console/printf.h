#pragma once
#include <colors.h>
#include <stdarg.h>

struct printf_cursor;
struct limine_framebuffer;

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

void k_info_impl(const char *category, int level, const char *file, int line,
                 const char *fmt, ...);
void k_printf(const char *format, ...);
void v_k_printf(struct printf_cursor *csr, const char *format, va_list args);
void serial_init();
void k_printf_init(struct limine_framebuffer *fb);

#define k_info(category, level, fmt, ...)                                      \
    k_info_impl(category, level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
