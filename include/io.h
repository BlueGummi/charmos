#include <stdint.h>

// =========| IN - BYTE, WORD, LONG, QWORD, SB, SW, SL, SQ |=========

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint64_t inq(uint16_t port) {
    uint64_t ret;
    asm volatile("inq %1, %0" : "=A"(ret) : "Nd"(port));
    return ret;
}

static inline void insb(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insb" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void insw(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void insl(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void insq(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insq" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

// ========| OUT - BYTE, WORD, LONG, QWORD, SB, SW, SL, SQ |========

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %1, %0" ::"dN"(port), "a"(value));
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outq(uint16_t port, uint64_t value) {
    asm volatile("outq %0, %1" : : "A"(value), "Nd"(port));
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
    asm volatile("rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsb(uint16_t port, const void *addr, uint32_t count) {
    asm volatile("rep outsb" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsl(uint16_t port, const void *addr, uint32_t count) {
    asm volatile("rep outsl" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsq(uint16_t port, const void *addr, uint32_t count) {
    asm volatile("rep outsq" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

#pragma once
