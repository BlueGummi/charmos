#pragma once
#include <colors.h>
#include <sch/irql.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

struct printf_cursor;
struct limine_framebuffer;

void printf(const char *format, ...);
void vprintf(struct printf_cursor *csr, const char *format, va_list args);
void serial_init();
void serial_write(const char *str, size_t len);
bool serial_try_getc(char *out);
void printf_init(struct limine_framebuffer *fb);
void printf_unlocked(const char *format, ...);
void printf_unlock(enum irql i);
enum irql printf_lock();
