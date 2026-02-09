#include <asm.h>
#include <limine.h>
#include <limits.h>
#include <logo.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "console/printf.h"
#include <flanterm/src/flanterm_backends/fb.h>
#include "flanterm/src/flanterm.h"

struct flanterm_context;

struct spinlock k_printf_lock = SPINLOCK_INIT;
struct flanterm_context *ft_ctx;

struct printf_cursor {
    char *buffer;
    int buffer_len;
    int cursor;
};

void serial_init() {
    outb(0x3F8 + 1, 0x00);

    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);

    outb(0x3F8 + 2, 0xC7);

    outb(0x3F8 + 4, 0x0B);

    for (volatile int i = 0; i < 1000; i++)
        cpu_relax();
}

static int serial_is_transmit_empty() {
    return inb(0x3F8 + 5) & 0x20;
}

static void serial_putc(char c) {
    while (serial_is_transmit_empty() == 0)
        ;

    outb(0x3F8, (uint8_t) c);
}

void serial_puts(struct printf_cursor *csr, const char *str, int len) {
    for (int i = 0; i < len; i++) {
        if (!csr)
            serial_putc(str[i]);

        if (csr && csr->cursor < csr->buffer_len - 1) {
            if (csr->buffer)
                csr->buffer[csr->cursor] = str[i];

            csr->cursor++;
        }
    }
}

void double_print(struct flanterm_context *f, struct printf_cursor *csr,
                  const char *str, int len) {
    (void) f;
    serial_puts(csr, str, len);
    if (global.current_bootstage >= BOOTSTAGE_EARLY_FB)
        flanterm_write(f, str, len);
}

void k_printf_init(struct limine_framebuffer *fb) {
    (void) fb;
    serial_init();
    ft_ctx = flanterm_fb_init(
        NULL, NULL, fb->address, fb->width, fb->height, fb->pitch,
        fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
        fb->green_mask_shift, fb->blue_mask_size, fb->blue_mask_shift, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 1, 0, 0, 0);

    k_printf("%s", OS_LOGO_SMALL);
}

static int print_signed(char *buffer, int64_t num) {
    int neg = 0;
    int n = 0;

    if (num < 0) {
        neg = 1;
        num = -num;
    }

    do {
        buffer[n++] = '0' + (num % 10);
        num /= 10;
    } while (num > 0);

    for (int i = 0; i < n / 2; i++) {
        char tmp = buffer[i];
        buffer[i] = buffer[n - 1 - i];
        buffer[n - 1 - i] = tmp;
    }

    if (neg) {
        memmove(buffer + 1, buffer, n);
        buffer[0] = '-';
        n++;
    }

    return n;
}

static int print_unsigned(char *buffer, uint64_t num) {
    int n = 0;

    do {
        buffer[n++] = '0' + (num % 10);
        num /= 10;
    } while (num > 0);

    for (int i = 0; i < n / 2; i++) {
        char tmp = buffer[i];
        buffer[i] = buffer[n - 1 - i];
        buffer[n - 1 - i] = tmp;
    }

    return n;
}

static int print_hex(char *buffer, uint64_t num) {
    const char *digits = "0123456789abcdef";
    int n = 0;

    do {
        buffer[n++] = digits[num % 16];
        num /= 16;
    } while (num > 0);

    for (int i = 0; i < n / 2; i++) {
        char tmp = buffer[i];
        buffer[i] = buffer[n - 1 - i];
        buffer[n - 1 - i] = tmp;
    }

    return n;
}

static int print_hex_upper(char *buffer, uint64_t num) {
    const char *digits = "0123456789ABCDEF";
    int n = 0;

    do {
        buffer[n++] = digits[num % 16];
        num /= 16;
    } while (num > 0);

    for (int i = 0; i < n / 2; i++) {
        char tmp = buffer[i];
        buffer[i] = buffer[n - 1 - i];
        buffer[n - 1 - i] = tmp;
    }

    return n;
}
static int print_binary(char *buffer, uint64_t num) {
    const char *digits = "01";
    int n = 0;

    if (num == 0) {
        buffer[n++] = '0';
    } else {
        while (num > 0) {
            buffer[n++] = digits[num % 2];
            num /= 2;
        }
        for (int i = 0; i < n / 2; i++) {
            char tmp = buffer[i];
            buffer[i] = buffer[n - 1 - i];
            buffer[n - 1 - i] = tmp;
        }
    }

    return n;
}

static int print_octal(char *buffer, uint64_t num) {
    const char *digits = "01234567";
    int n = 0;

    if (num == 0) {
        buffer[n++] = '0';
        return n;
    }

    while (num > 0) {
        buffer[n++] = digits[num % 8];
        num /= 8;
    }

    for (int i = 0; i < n / 2; i++) {
        char tmp = buffer[i];
        buffer[i] = buffer[n - 1 - i];
        buffer[n - 1 - i] = tmp;
    }

    return n;
}

static void apply_padding(const char *str, int len, int width, bool left_align,
                          bool zero_pad, struct printf_cursor *csr) {
    if (len >= width) {
        double_print(ft_ctx, csr, str, len);
        return;
    }

    int padding = width - len;
    char pad_char = zero_pad ? '0' : ' ';

    if (!left_align) {
        if (zero_pad && len > 0 && (str[0] == '-' || str[0] == '+')) {
            double_print(ft_ctx, csr, str, 1);
            for (int i = 0; i < padding; i++)
                double_print(ft_ctx, csr, &pad_char, 1);
            double_print(ft_ctx, csr, str + 1, len - 1);
        } else {
            for (int i = 0; i < padding; i++)
                double_print(ft_ctx, csr, &pad_char, 1);
            double_print(ft_ctx, csr, str, len);
        }
    } else {
        double_print(ft_ctx, csr, str, len);
        for (int i = 0; i < padding; i++)
            double_print(ft_ctx, csr, " ", 1);
    }
}

static void handle_format_specifier(struct printf_cursor *csr,
                                    const char **format_ptr, va_list args) {
    const char *format = *format_ptr;
    bool left_align = false;
    bool zero_pad = false;
    int width = 0;

    // Align
    while (*format == '-' || *format == '+' || *format == '0' ||
           *format == ' ' || *format == '#') {
        if (*format == '-')
            left_align = true;
        else if (*format == '0')
            zero_pad = true;
        format++;
    }

    if (*format >= '0' && *format <= '9') {
        width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }
    }

    // Width
    enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z } len_mod = LEN_NONE;
    if (*format == 'z') {
        len_mod = LEN_Z;
        format++;
    } else if (*format == 'h') {
        format++;
        if (*format == 'h') {
            len_mod = LEN_HH;
            format++;
        } else {
            len_mod = LEN_H;
        }
    } else if (*format == 'l') {
        format++;
        if (*format == 'l') {
            len_mod = LEN_LL;
            format++;
        } else {
            len_mod = LEN_L;
        }
    }

    char spec = *format++;
    char buffer[64];
    int len = 0;

    // Style
    switch (spec) {
    case 'd':
    case 'i': {
        int64_t num;
        switch (len_mod) {
        case LEN_HH: num = (signed char) va_arg(args, int); break;
        case LEN_H: num = (short) va_arg(args, int); break;
        case LEN_L: num = va_arg(args, long); break;
        case LEN_LL: num = va_arg(args, long long); break;
        case LEN_Z: num = (int64_t) va_arg(args, uint64_t); break;
        default: num = va_arg(args, int); break;
        }
        len = print_signed(buffer, num);
        break;
    }
    case 'u': {
        uint64_t num;
        switch (len_mod) {
        case LEN_HH: num = (unsigned char) va_arg(args, unsigned int); break;
        case LEN_H: num = (unsigned short) va_arg(args, unsigned int); break;
        case LEN_L: num = va_arg(args, unsigned long); break;
        case LEN_LL: num = va_arg(args, unsigned long long); break;
        case LEN_Z: num = va_arg(args, uint64_t); break;
        default: num = va_arg(args, unsigned int); break;
        }
        len = print_unsigned(buffer, num);
        break;
    }
    case 'x': {
        uint64_t num;
        switch (len_mod) {
        case LEN_HH: num = (unsigned char) va_arg(args, unsigned int); break;
        case LEN_H: num = (unsigned short) va_arg(args, unsigned int); break;
        case LEN_L: num = va_arg(args, unsigned long); break;
        case LEN_LL: num = va_arg(args, unsigned long long); break;
        case LEN_Z: num = va_arg(args, uint64_t); break;
        default: num = va_arg(args, unsigned int); break;
        }
        len = print_hex(buffer, num);
        break;
    }
    case 'X': {
        uint64_t num;
        switch (len_mod) {
        case LEN_HH: num = (unsigned char) va_arg(args, unsigned int); break;
        case LEN_H: num = (unsigned short) va_arg(args, unsigned int); break;
        case LEN_L: num = va_arg(args, unsigned long); break;
        case LEN_LL: num = va_arg(args, unsigned long long); break;
        case LEN_Z: num = va_arg(args, uint64_t); break;
        default: num = va_arg(args, unsigned int); break;
        }
        len = print_hex_upper(buffer, num);
        break;
    }
    case 'b': {
        uint64_t num;
        switch (len_mod) {
        case LEN_HH: num = (unsigned char) va_arg(args, unsigned int); break;
        case LEN_H: num = (unsigned short) va_arg(args, unsigned int); break;
        case LEN_L: num = va_arg(args, unsigned long); break;
        case LEN_LL: num = va_arg(args, unsigned long long); break;
        case LEN_Z: num = va_arg(args, uint64_t); break;
        default: num = va_arg(args, unsigned int); break;
        }
        len = print_binary(buffer, num);
        break;
    }
    case 'o': {
        uint64_t num;
        switch (len_mod) {
        case LEN_HH: num = (unsigned char) va_arg(args, unsigned int); break;
        case LEN_H: num = (unsigned short) va_arg(args, unsigned int); break;
        case LEN_L: num = va_arg(args, unsigned long); break;
        case LEN_LL: num = va_arg(args, unsigned long long); break;
        case LEN_Z: num = va_arg(args, uint64_t); break;
        default: num = va_arg(args, unsigned int); break;
        }
        len = print_octal(buffer, num);
        break;
    }
    case 's': {
        char *str = va_arg(args, char *);
        len = strlen(str);
        apply_padding(str, len, width, left_align, false, csr);
        *format_ptr = format;
        return;
    }
    case 'c': {
        buffer[0] = (char) va_arg(args, int);
        len = 1;
        zero_pad = false;
        break;
    }
    case '%': {
        buffer[0] = '%';
        len = 1;
        zero_pad = false;
        break;
    }
    default: {
        buffer[0] = '%';
        buffer[1] = spec;
        len = 2;
        zero_pad = false;
        break;
    }
    }

    apply_padding(buffer, len, width, left_align, zero_pad, csr);
    *format_ptr = format;
}

void v_k_printf(struct printf_cursor *csr, const char *format, va_list args) {
    while (*format) {
        if (*format == '%') {
            format++;
            handle_format_specifier(csr, &format, args);
        } else {
            if (*format == '\n') {
                double_print(ft_ctx, csr, "\n", 1);
            } else {
                double_print(ft_ctx, csr, format, 1);
            }
            format++;
        }
    }
}

void k_printf(const char *format, ...) {
    bool i = are_interrupts_enabled();
    disable_interrupts();
    spin_lock_raw(&k_printf_lock);
    va_list args;
    va_start(args, format);
    v_k_printf(NULL, format, args);
    va_end(args);
    spin_unlock_raw(&k_printf_lock);

    if (i)
        enable_interrupts();
}

int vsnprintf(char *buffer, int buffer_len, const char *format, va_list args) {

    if (!buffer_len)
        buffer_len = INT_MAX;

    struct printf_cursor csr = {
        .buffer = buffer,
        .buffer_len = buffer_len,
        .cursor = 0,
    };

    v_k_printf(&csr, format, args);

    if (buffer)
        csr.buffer[csr.cursor] = '\0';

    return csr.cursor;
}

int snprintf(char *buffer, int buffer_len, const char *format, ...) {
    va_list args;
    va_start(args, format);

    if (!buffer_len)
        buffer_len = INT_MAX;

    struct printf_cursor csr = {
        .buffer = buffer,
        .buffer_len = buffer_len,
        .cursor = 0,
    };

    v_k_printf(&csr, format, args);
    va_end(args);

    if (buffer)
        csr.buffer[csr.cursor] = '\0';

    return csr.cursor;
}

void panic(const char *format, ...) {
    va_list args;
    va_start(args, format);
    v_k_printf(NULL, format, args);
    va_end(args);

    while (true) {
        disable_interrupts();
        wait_for_interrupt();
    }
}
