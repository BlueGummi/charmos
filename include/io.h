#include <stdint.h>
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t value);
uint16_t inw(uint16_t port);
static inline void insw(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}
#pragma once
