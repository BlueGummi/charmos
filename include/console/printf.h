#pragma once
#include <colors.h>
#include <stdarg.h>

struct printf_cursor;
struct limine_framebuffer;

void k_info_impl(const char *category, int level, const char *file, int line,
                 const char *fmt, ...);
void k_printf(const char *format, ...);
void k_vprintf(struct printf_cursor *csr, const char *format, va_list args);
void serial_init();
void k_printf_init(struct limine_framebuffer *fb);
