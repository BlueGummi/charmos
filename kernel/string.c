#include <limits.h>
#include <mem/alloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#undef memset
#undef memcpy
#undef memmove
#undef memcmp

void *memcpy(void *dest, const void *src, size_t n) {
    return __memcpy_inline(dest, src, n);
}

void *memset(void *s, int c, size_t n) {
    return __memset_inline(s, c, n);
}

void *memmove(void *dest, const void *src, size_t n) {
    return __memmove_inline(dest, src, n);
}

int memcmp(const void *s1, const void *s2, size_t n) {
    return __memcmp_inline(s1, s2, n);
}
size_t strlen(const char *str) {
    size_t length = 0;

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

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return s1[i] < s2[i] ? -1 : 1;
        }
    }
    return 0;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *original_dest = dest;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return original_dest;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *) s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t) c) {
            return (void *) (p + i);
        }
    }
    return NULL;
}

void *memrchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *) s;
    for (size_t i = n; i > 0; i--) {
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

int isupper(int c) {
    return (unsigned) c - 'A' < 26;
}

int isdigit(int c) {
    return (unsigned) c - '0' < 10;
}

int isalpha(int c) {
    return islower(c) || isupper(c);
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

int isprint(int c) {
    return (unsigned) c - 0x20 < 0x5f;
}

int toupper(int c) {
    if (islower(c))
        return c & 0x5f;
    return c;
}

int tolower(int c) {
    if (isupper(c))
        return c | 0x20;
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

    for (size_t i = 0; i <= len; i++)
        copy[i] = str[i];

    return copy;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    do {
        if (*s == (char) c)
            last = s;
    } while (*s++);
    return (char *) last;
}

size_t strspn(const char *s, const char *accept) {
    size_t i = 0;
    while (s[i]) {
        const char *a = accept;
        bool found = false;
        while (*a) {
            if (s[i] == *a++) {
                found = true;
                break;
            }
        }
        if (!found)
            break;
        i++;
    }
    return i;
}

size_t strcspn(const char *s, const char *reject) {
    size_t i = 0;
    while (s[i]) {
        const char *r = reject;
        while (*r) {
            if (s[i] == *r++)
                goto done;
        }
        i++;
    }
done:
    return i;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a++)
                return (char *) s;
        }
        s++;
    }
    return NULL;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle)
        return (char *) haystack;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen)
        return NULL;

    // Build KMP failure table
    int64_t table[nlen];
    table[0] = -1;
    int64_t k = -1;
    for (size_t i = 1; i < nlen; i++) {
        while (k >= 0 && needle[k + 1] != needle[i])
            k = table[k];
        if (needle[k + 1] == needle[i])
            k++;
        table[i] = k;
    }

    int64_t q = -1;
    for (size_t i = 0; i < hlen; i++) {
        while (q >= 0 && needle[q + 1] != haystack[i])
            q = table[q];
        if (needle[q + 1] == haystack[i])
            q++;
        if (q == (int64_t) nlen - 1)
            return (char *) (haystack + i - nlen + 1);
    }
    return NULL;
}

char *strtok(char *str, const char *delim) {
    static char *saved = NULL;
    if (str)
        saved = str;
    if (!saved || !*saved)
        return NULL;

    saved += strspn(saved, delim);
    if (!*saved) {
        saved = NULL;
        return NULL;
    }

    char *token_start = saved;
    saved += strcspn(saved, delim);
    if (*saved)
        *saved++ = '\0';
    else
        saved = NULL;
    return token_start;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (str)
        *saveptr = str;
    if (!*saveptr || !**saveptr)
        return NULL;

    *saveptr += strspn(*saveptr, delim);
    if (!**saveptr) {
        *saveptr = NULL;
        return NULL;
    }

    char *token_start = *saveptr;
    *saveptr += strcspn(*saveptr, delim);
    if (**saveptr)
        *(*saveptr)++ = '\0';
    else
        *saveptr = NULL;
    return token_start;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d)
        d++;
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dest;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen && s[i] != '\0')
        i++;
    return i;
}

int64_t atoi(const char *str) {
    while (isspace((unsigned char) *str))
        str++;
    int64_t sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+')
        str++;
    int64_t result = 0;
    while (isdigit((unsigned char) *str))
        result = result * 10 + (*str++ - '0');
    return sign * result;
}

size_t atoui(const char *str) {
    while (isspace((unsigned char) *str))
        str++;
    if (*str == '+')
        str++;
    size_t result = 0;
    while (isdigit((unsigned char) *str))
        result = result * 10 + (*str++ - '0');
    return result;
}

size_t atohex(const char *str) {
    while (isspace((unsigned char) *str))
        str++;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
        str += 2;
    size_t result = 0;
    while (1) {
        char c = *str++;
        if (c >= '0' && c <= '9')
            result = result * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
            result = result * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            result = result * 16 + (c - 'A' + 10);
        else
            break;
    }
    return result;
}

char *itoa(int64_t value, char *buf, int base) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    char *p = buf;
    char *start;
    bool negative = (base == 10 && value < 0);
    size_t uval = negative ? -(size_t) value : (size_t) value;

    do {
        *p++ = digits[uval % base];
        uval /= base;
    } while (uval);

    if (negative)
        *p++ = '-';
    *p = '\0';

    start = buf;
    char *end = p - 1;
    while (start < end) {
        char tmp = *start;
        *start++ = *end;
        *end-- = tmp;
    }
    return buf;
}

char *utoa(size_t value, char *buf, int base) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    char *p = buf;
    char *start;
    do {
        *p++ = digits[value % base];
        value /= base;
    } while (value);
    *p = '\0';

    start = buf;
    char *end = p - 1;
    while (start < end) {
        char tmp = *start;
        *start++ = *end;
        *end-- = tmp;
    }
    return buf;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int d = tolower((unsigned char) *s1) - tolower((unsigned char) *s2);
        if (d)
            return d;
        s1++;
        s2++;
    }
    return tolower((unsigned char) *s1) - tolower((unsigned char) *s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n-- && *s1 && *s2) {
        int d = tolower((unsigned char) *s1) - tolower((unsigned char) *s2);
        if (d)
            return d;
        s1++;
        s2++;
    }
    if (n == (size_t) -1)
        return 0;
    return tolower((unsigned char) *s1) - tolower((unsigned char) *s2);
}

char *strrev(char *s) {
    char *l = s, *r = s + strlen(s) - 1;
    while (l < r) {
        char t = *l;
        *l++ = *r;
        *r-- = t;
    }
    return s;
}

char *strtoupper(char *s) {
    for (char *p = s; *p; p++)
        *p = (char) toupper((unsigned char) *p);
    return s;
}

char *strtolower(char *s) {
    for (char *p = s; *p; p++)
        *p = (char) tolower((unsigned char) *p);
    return s;
}

char *strndup(const char *str, size_t n) {
    if (!str)
        return NULL;
    size_t len = strnlen(str, n);
    char *copy = (char *) kmalloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;

    while (*s && isspace((unsigned char) *s)) {
        s++;
    }

    int neg = 0;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }

    if (base != 0 && (base < 2 || base > 36)) {
        if (endptr)
            *endptr = (char *) nptr;
        return 0;
    }

    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s += 1;
            }
        } else {
            base = 10;
        }
    } else {
        if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
    }

    const char *start_digits = s;

    unsigned long acc = 0;

    unsigned long limit =
        neg ? (unsigned long) LONG_MAX + 1UL : (unsigned long) LONG_MAX;

    for (;;) {
        int digit;

        char c = *s;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'Z')
            digit = 10 + (c - 'A');
        else
            break;

        if (digit >= base)
            break;

        if (acc > (limit - (unsigned long) digit) / (unsigned long) base) {
            while (1) {
                s++;
                char dch = *s;
                if (dch >= '0' && dch <= '9')
                    digit = dch - '0';
                else if (dch >= 'a' && dch <= 'z')
                    digit = 10 + (dch - 'a');
                else if (dch >= 'A' && dch <= 'Z')
                    digit = 10 + (dch - 'A');
                else
                    break;
                if (digit >= base)
                    break;
            }
            break;
        }

        acc = acc * (unsigned long) base + (unsigned long) digit;
        s++;
    }

    if (endptr)
        *endptr = (char *) s;

    if (s == start_digits) {
        return 0;
    }

    if (neg) {
        if (acc == (unsigned long) LONG_MAX + 1UL)
            return LONG_MIN;
        return -(long) acc;
    } else {
        return (long) acc;
    }
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *s = nptr;

    while (*s && isspace((unsigned char) *s))
        s++;

    int neg = 0;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }

    if (base != 0 && (base < 2 || base > 36)) {
        if (endptr)
            *endptr = (char *) nptr;
        return 0;
    }

    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (*s == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    const char *start_digits = s;
    unsigned long long acc = 0;
    int overflow = 0;

    for (;;) {
        int digit;
        char c = *s;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'Z')
            digit = 10 + (c - 'A');
        else
            break;

        if (digit >= base)
            break;

        if (overflow || acc > (ULLONG_MAX - (unsigned long long) digit) /
                                  (unsigned long long) base)
            overflow = 1; /* keep consuming digits so endptr stays correct */
        else
            acc = acc * (unsigned long long) base + (unsigned long long) digit;

        s++;
    }

    if (endptr)
        *endptr = (char *) (s == start_digits ? nptr : s);

    if (s == start_digits)
        return 0;

    if (overflow)
        return ULLONG_MAX;

    return neg ? (0ULL - acc) : acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long) strtoull(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long) strtol(nptr, endptr, base);
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = strlen(src);
    if (size) {
        size_t n = (srclen < size - 1) ? srclen : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dstlen = strnlen(dst, size);
    size_t srclen = strlen(src);

    if (dstlen == size)
        return size + srclen;

    size_t avail = size - dstlen;
    size_t n = (srclen < avail - 1) ? srclen : avail - 1;
    memcpy(dst + dstlen, src, n);
    dst[dstlen + n] = '\0';
    return dstlen + srclen;
}

char *strsep(char **stringp, const char *delim) {
    char *start = *stringp;
    if (!start)
        return NULL;

    char *p = strpbrk(start, delim);
    if (p) {
        *p = '\0';
        *stringp = p + 1;
    } else {
        *stringp = NULL;
    }
    return start;
}

char *strchrnul(const char *s, int c) {
    while (*s && *s != (char) c)
        s++;
    return (char *) s;
}
