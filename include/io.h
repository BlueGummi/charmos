#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

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

// ============| PCI |==============

static inline uint32_t pci_config_address(uint8_t bus, uint8_t slot,
                                          uint8_t func, uint8_t offset) {
    return (uint32_t) ((1U << 31) | (bus << 16) | (slot << 11) | (func << 8) |
                       (offset & 0xFC));
}

static inline uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static inline uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func,
                                     uint8_t offset) {
    uint32_t value = pci_read(bus, slot, func, offset & 0xFC);
    return (value >> ((offset & 2) * 8)) & 0xFFFF;
}

static inline uint8_t pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint8_t offset) {
    uint32_t value = pci_read(bus, slot, func, offset & 0xFC);
    return (value >> ((offset & 3) * 8)) & 0xFF;
}

static inline void pci_write(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static inline void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset, uint16_t value) {
    uint32_t tmp = pci_read(bus, slot, func, offset & 0xFC);
    uint32_t shift = (offset & 2) * 8;
    tmp = (tmp & ~(0xFFFF << shift)) | ((uint32_t) value << shift);
    pci_write(bus, slot, func, offset & 0xFC, tmp);
}

static inline void pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset, uint8_t value) {
    uint32_t tmp = pci_read(bus, slot, func, offset & 0xFC);
    uint32_t shift = (offset & 3) * 8;
    tmp = (tmp & ~(0xFF << shift)) | ((uint32_t) value << shift);
    pci_write(bus, slot, func, offset & 0xFC, tmp);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}


#pragma once
