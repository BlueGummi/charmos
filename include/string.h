#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);

#define __STRING_FAST static inline __attribute__((always_inline, artificial))

__STRING_FAST void *__memset_inline(void *s, int c, size_t n) {
    void *ret = s;
    uint64_t word = (uint64_t) (uint8_t) c * 0x0101010101010101ULL;
    size_t qwords = n >> 3;
    size_t rem = n & 7;

    asm volatile("rep stosq" : "+D"(s), "+c"(qwords) : "a"(word) : "memory");
    asm volatile("rep stosb" : "+D"(s), "+c"(rem) : "a"(word) : "memory");

    return ret;
}

__STRING_FAST void *__memcpy_inline(void *dest, const void *src, size_t n) {
    void *ret = dest;
    size_t qwords = n >> 3;
    size_t rem = n & 7;

    asm volatile("rep movsq"
                 : "+D"(dest), "+S"(src), "+c"(qwords)
                 :
                 : "memory");
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(rem) : : "memory");

    return ret;
}

__STRING_FAST void *__memmove_inline(void *dest, const void *src, size_t n) {
    if ((uintptr_t) dest - (uintptr_t) src >= n)
        return __memcpy_inline(dest, src, n);

    uint8_t *d = (uint8_t *) dest + n;
    const uint8_t *s = (const uint8_t *) src + n;
    while (n--)
        *--d = *--s;

    return dest;
}

__STRING_FAST int __memcmp_inline(const void *s1, const void *s2, size_t n) {
    if (!n)
        return 0;

    const unsigned char *p1 = (const unsigned char *) s1;
    const unsigned char *p2 = (const unsigned char *) s2;

    asm volatile("repe cmpsb" : "+D"(p1), "+S"(p2), "+c"(n) : : "memory", "cc");

    return (int) p1[-1] - (int) p2[-1];
}

#define memset(s, c, n) __memset_inline((s), (c), (n))
#define memcpy(d, s, n) __memcpy_inline((d), (s), (n))
#define memmove(d, s, n) __memmove_inline((d), (s), (n))
#define memcmp(a, b, n) __memcmp_inline((a), (b), (n))

size_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
int strncmp(const char *s1, const char *s2, size_t n);
int strcmp(const char *str1, const char *str2);
char *strncpy(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strstr(const char *haystack, const char *needle);

char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);

char *strncat(char *dest, const char *src, size_t n);
size_t strnlen(const char *s, size_t maxlen);

int islower(int c);
int isupper(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

char *strdup(const char *str);
char *strndup(const char *str, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

char *strrev(char *s);
char *strtoupper(char *s);
char *strtolower(char *s);

int64_t atoi(const char *str);
size_t atoui(const char *str);
size_t atohex(const char *str);

char *itoa(int64_t value, char *buf, int base);
char *utoa(size_t value, char *buf, int base);

long strtol(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char *strsep(char **stringp, const char *delim);
char *strchrnul(const char *s, int c);

int vsnprintf(char *buffer, int buffer_len, const char *format, va_list args);
int snprintf(char *buffer, int buffer_len, const char *format, ...);
int vasprintf(char **strp, const char *fmt, va_list args);
int asprintf(char **strp, const char *fmt, ...);
