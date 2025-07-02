#include <mem/alloc.h>
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, uint64_t n) {
    uint8_t *pdest = (uint8_t *) dest;
    const uint8_t *psrc = (const uint8_t *) src;

    for (uint64_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *) s;

    for (uint64_t i = 0; i < n; i++) {
        p[i] = (uint8_t) c;
    }

    return s;
}

void *memmove(void *dest, const void *src, uint64_t n) {
    uint8_t *pdest = (uint8_t *) dest;
    const uint8_t *psrc = (const uint8_t *) src;

    if (src > dest) {
        for (uint64_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (uint64_t i = n; i > 0; i--) {
            pdest[i - 1] = psrc[i - 1];
        }
    }

    return dest;
}

int memcmp(const void *s1, const void *s2, uint64_t n) {
    const uint8_t *p1 = (const uint8_t *) s1;
    const uint8_t *p2 = (const uint8_t *) s2;

    for (uint64_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}
uint64_t strlen(const char *str) {
    uint64_t length = 0;

    while (str[length] != '\0') {
        length++;
    }

    return length;
}

char *strcpy(char *dest, const char *src) {
    char *original_dest = dest;
    while ((*dest++ = *src++))
        ;
    return original_dest;
}

char *strcat(char *dest, const char *src) {
    char *original_dest = dest;
    while (*dest) {
        dest++;
    }
    while ((*dest++ = *src++))
        ;

    return original_dest;
}

int strncmp(const char *s1, const char *s2, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return s1[i] < s2[i] ? -1 : 1;
        }
    }
    return 0;
}

char *strncpy(char *dest, const char *src, uint64_t n) {
    char *original_dest = dest;
    uint64_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return original_dest;
}

void *memchr(const void *s, int c, uint64_t n) {
    const uint8_t *p = (const uint8_t *) s;
    for (uint64_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t) c) {
            return (void *) (p + i);
        }
    }
    return NULL;
}

void *memrchr(const void *s, int c, uint64_t n) {
    const uint8_t *p = (const uint8_t *) s;
    for (uint64_t i = n; i > 0; i--) {
        if (p[i - 1] == (uint8_t) c) {
            return (void *) (p + (i - 1));
        }
    }
    return NULL;
}

int strcmp(const char *str1, const char *str2) {
    while (*str1 != '\0' && *str2 != '\0') {
        if (*str1 != *str2) {
            return (unsigned char) (*str1) - (unsigned char) (*str2);
        }
        str1++;
        str2++;
    }
    return (unsigned char) (*str1) - (unsigned char) (*str2);
}

char *strchr(const char *s, int c) {
    do {
        if (*s == c) {
            return (char *) s;
        }
    } while (*s++);
    return (0);
}

int islower(int c) {
    return (unsigned) c - 'a' < 26;
}

int toupper(int c) {
    if (islower(c))
        return c & 0x5f;
    return c;
}

char *strdup(const char *str) {
    if (!str)
        return NULL;

    size_t len = 0;
    while (str[len] != '\0')
        len++;

    char *copy = (char *) kmalloc(len + 1);
    if (!copy)
        return NULL;

    for (size_t i = 0; i <= len; ++i)
        copy[i] = str[i];

    return copy;
}

#include <stdarg.h>
#include <stdbool.h>

static void reverse_str(char *str, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char tmp = str[i];
        str[i] = str[j];
        str[j] = tmp;
        i++;
        j--;
    }
}

static int int_to_str(int val, char *buf, int bufsize) {
    if (bufsize == 0)
        return 0;

    bool negative = false;
    unsigned int uval;

    if (val < 0) {
        negative = true;
        uval = (unsigned int) (-val);
    } else {
        uval = (unsigned int) val;
    }

    int i = 0;
    if (uval == 0) {
        if (i < bufsize - 1)
            buf[i++] = '0';
    } else {
        while (uval && i < bufsize - 1) {
            buf[i++] = (uval % 10) + '0';
            uval /= 10;
        }
    }

    if (negative && i < bufsize - 1) {
        buf[i++] = '-';
    }

    reverse_str(buf, i);
    buf[i] = '\0';
    return i;
}

int snprintf(char *buffer, int buffer_len, const char *format, ...) {
    if (buffer_len <= 0)
        return 0;

    va_list args;
    va_start(args, format);

    int pos = 0;
    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%' && format[i + 1] != '\0') {
            i++;
            if (format[i] == 'd') {
                int val = va_arg(args, int);
                char numbuf[32];
                int len = int_to_str(val, numbuf, sizeof(numbuf));
                for (int j = 0; j < len && pos < buffer_len - 1; j++) {
                    buffer[pos++] = numbuf[j];
                }
            } else if (format[i] == 's') {
                const char *str = va_arg(args, const char *);
                if (!str)
                    str = "(null)";
                for (int j = 0; str[j] != '\0' && pos < buffer_len - 1; j++) {
                    buffer[pos++] = str[j];
                }
            } else {
                // Unsupported format, print literally
                if (pos < buffer_len - 1)
                    buffer[pos++] = '%';
                if (pos < buffer_len - 1)
                    buffer[pos++] = format[i];
            }
        } else {
            if (pos < buffer_len - 1)
                buffer[pos++] = format[i];
        }
    }

    buffer[pos] = '\0';
    va_end(args);
    return pos; // number of chars written (excluding null)
}
