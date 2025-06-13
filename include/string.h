#include <stddef.h>
#include <stdint.h>
void *memcpy(void *dest, const void *src, uint64_t n);
void *memset(void *s, int c, uint64_t n);
void *memmove(void *dest, const void *src, uint64_t n);
int memcmp(const void *s1, const void *s2, uint64_t n);
uint64_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
int strncmp(const char *s1, const char *s2, uint64_t n);
char *strncpy(char *dest, const char *src, uint64_t n);
void *memchr(const void *s, int c, uint64_t n);
void *memrchr(const void *s, int c, uint64_t n);
int strcmp(const char *str1, const char *str2);
char *strchr(const char *s, int c);
int islower(int c);
int toupper(int c);
int snprintf(char *buffer, int buffer_len, const char *format, ...);
#pragma once
