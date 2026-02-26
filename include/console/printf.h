#pragma once
#include <colors.h>
#include <stdarg.h>

struct printf_cursor;
struct limine_framebuffer;

void printf(const char *format, ...);
void vprintf(struct printf_cursor *csr, const char *format, va_list args);
void serial_init();
void printf_init(struct limine_framebuffer *fb);
