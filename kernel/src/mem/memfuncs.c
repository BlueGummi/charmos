#include <stddef.h>
#include <stdint.h>
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *) dest;
    const uint8_t *psrc = (const uint8_t *) src;

    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *) s;

    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t) c;
    }

    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *) dest;
    const uint8_t *psrc = (const uint8_t *) src;

    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i - 1] = psrc[i - 1];
        }
    }

    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *) s1;
    const uint8_t *p2 = (const uint8_t *) s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
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
