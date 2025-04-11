#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <stdarg.h>
#include <stddef.h>
#include <system/memfuncs.h>

struct flanterm_context *ft_ctx;

void k_printf_init(struct flanterm_context *f) {
    ft_ctx = f;
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

static void apply_padding(const char *str, int len, int width, bool left_align,
                          bool zero_pad) {
    if (len >= width) {
        flanterm_write(ft_ctx, str, len);
        return;
    }

    int padding = width - len;
    char pad_char = zero_pad ? '0' : ' ';

    if (!left_align) {
        if (zero_pad && len > 0 && (str[0] == '-' || str[0] == '+')) {
            flanterm_write(ft_ctx, str, 1);
            for (int i = 0; i < padding; i++)
                flanterm_write(ft_ctx, &pad_char, 1);
            flanterm_write(ft_ctx, str + 1, len - 1);
        } else {
            for (int i = 0; i < padding; i++)
                flanterm_write(ft_ctx, &pad_char, 1);
            flanterm_write(ft_ctx, str, len);
        }
    } else {
        flanterm_write(ft_ctx, str, len);
        for (int i = 0; i < padding; i++)
            flanterm_write(ft_ctx, " ", 1);
    }
}

static void handle_format_specifier(const char **format_ptr, va_list args) {
    const char *format = *format_ptr;
    bool left_align = false;
    bool zero_pad = false;
    int width = 0;

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
    char buffer[32];
    int len = 0;

    switch (spec) {
    case 'd':
    case 'i': {
        int64_t num;
        switch (len_mod) {
        case LEN_HH: num = (signed char) va_arg(args, int); break;
        case LEN_H: num = (short) va_arg(args, int); break;
        case LEN_L: num = va_arg(args, long); break;
        case LEN_LL: num = va_arg(args, long long); break;
        case LEN_Z: num = (int64_t) va_arg(args, size_t); break;
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
        case LEN_Z: num = va_arg(args, size_t); break;
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
        case LEN_Z: num = va_arg(args, size_t); break;
        default: num = va_arg(args, unsigned int); break;
        }
        len = print_hex(buffer, num);
        break;
    }
    case 's': {
        char *str = va_arg(args, char *);
        len = strlen(str);
        apply_padding(str, len, width, left_align, false);
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

    apply_padding(buffer, len, width, left_align, zero_pad);
    *format_ptr = format;
}

void k_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    while (*format) {
        if (*format == '%') {
            format++;
            handle_format_specifier(&format, args);
        } else {
            if (*format == '\n') {
                flanterm_write(ft_ctx, "\n", 1);
            } else {
                flanterm_write(ft_ctx, format, 1);
            }
            format++;
        }
    }

    va_end(args);
}

void panic(const char *format, ...) {
    va_list args;
    va_start(args, format);
    k_printf(format, args);
    va_end(args);
    while (1) {
        asm("hlt");
    }
}
