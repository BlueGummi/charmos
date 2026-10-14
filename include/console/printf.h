#pragma once
#include <colors.h>
#include <compiler/core.h>
#include <sch/irql.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <sync/spinlock.h>
#include <tsa.h>

struct printf_cursor;
struct limine_framebuffer;

void printf(const char *format, ...) cc_printf_like(1, 2) cc_nonnull(1);
void vprintf(struct printf_cursor *csr, const char *format, va_list args)
    cc_printf_like(2, 0) cc_nonnull(2);
void serial_init(void);
void serial_write(const char *str, size_t len) cc_nonnull(1);
bool serial_try_getc(char *out) cc_nonnull(1) cc_warn_unused_result;

extern struct spinlock k_printf_lock;

void printf_init(struct limine_framebuffer *fb) cc_nonnull(1);
void printf_unlocked(const char *format, ...) cc_printf_like(1, 2)
cc_nonnull(1);
void printf_unlock(enum irql i) TSA_RELEASES_SPIN(&k_printf_lock);
enum irql printf_lock(void) TSA_ACQUIRES_SPIN(&k_printf_lock);
