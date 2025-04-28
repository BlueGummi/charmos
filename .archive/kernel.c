#include <stdarg.h>

#define WHITE_TXT 0x07

void k_clear_screen();
unsigned int k_printf(const char *format, unsigned int line, ...);

void k_main() {
    k_clear_screen();
    k_printf("I like to make walks in the morning, ja? with my legs", 0);
    k_printf("Integer: %d, String: %s, Char: %c, Hex: %x\n", 1, 42, "test", 'A',
             0xDEADBEEF);
};

void k_clear_screen() {
    char *vidmem = (char *) 0xb8000;
    unsigned int i = 0;
    while (i < (80 * 25 * 2)) {
        vidmem[i] = ' ';
        i++;
        vidmem[i] = WHITE_TXT;
        i++;
    };
};

unsigned int k_printf(const char *format, unsigned int line, ...) {
    char *vidmem = (char *) 0xb8000;
    unsigned int i = (line * 80 * 2);
    va_list args;
    va_start(args, line);

    while (*format) {
        if (*format == '%') {
            format++;
            switch (*format) {
            case 'd': {
                int num = va_arg(args, int);
                char buffer[32];
                int n = 0;
                if (num < 0) {
                    vidmem[i++] = '-';
                    num = -num;
                }
                do {
                    buffer[n++] = '0' + (num % 10);
                    num /= 10;
                } while (num > 0);
                while (n--) {
                    vidmem[i++] = buffer[n];
                    vidmem[i++] = WHITE_TXT;
                }
                break;
            }
            case 's': {
                char *str = va_arg(args, char *);
                while (*str) {
                    vidmem[i++] = *str++;
                    vidmem[i++] = WHITE_TXT;
                }
                break;
            }
            case 'c': {
                char c = va_arg(args, int);
                vidmem[i++] = c;
                vidmem[i++] = WHITE_TXT;
                break;
            }
            case 'x': {
                unsigned int num = va_arg(args, unsigned int);
                char buffer[32];
                int n = 0;
                do {
                    int rem = num % 16;
                    buffer[n++] = (rem < 10) ? ('0' + rem) : ('a' + (rem - 10));
                    num /= 16;
                } while (num > 0);
                while (n--) {
                    vidmem[i++] = buffer[n];
                    vidmem[i++] = WHITE_TXT;
                }
                break;
            }
            case '%': {
                vidmem[i++] = '%';
                vidmem[i++] = WHITE_TXT;
                break;
            }
            default: {
                vidmem[i++] = '%';
                vidmem[i++] = WHITE_TXT;
                vidmem[i++] = *format;
                vidmem[i++] = WHITE_TXT;
                break;
            }
            }
        } else if (*format == '\n') {
            line++;
            i = (line * 80 * 2);
        } else {
            vidmem[i++] = *format;
            vidmem[i++] = WHITE_TXT;
        }
        format++;
    }

    va_end(args);
    return 1;
}
