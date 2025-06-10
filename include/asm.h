#include <console/printf.h>
#include <mp/core.h>
#include <stdbool.h>
#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

//
//
//
//
//
// =========| IN - BYTE, WORD, LONG, SB, SW, SL |=========
//
//
//
//
//

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

static inline void insb(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insb" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void insw(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void insl(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

//
//
//
//
//
// ========| OUT - BYTE, WORD, LONG, SB, SW, SL |========
//
//
//
//
//

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %1, %0" ::"dN"(port), "a"(value));
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
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

//
//
//
//
//
//
// =============================| PCI |===============================
//
//
//
//

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

//
//
//
//
//
// ========================| MMIO  |=========================
//
//
//
//
//

static inline void mmio_write_64(void *address, uint64_t value) {
    *(uint64_t *) address = value;
}

static inline void mmio_write_32(void *address, uint32_t value) {
    *(uint32_t *) address = value;
}

static inline void mmio_write_16(void *address, uint16_t value) {
    *(uint16_t *) address = value;
}

static inline void mmio_write_8(void *address, uint8_t value) {
    *(uint8_t *) address = value;
}

static inline uint64_t mmio_read_64(void *address) {
    return *(uint64_t *) address;
}

static inline uint32_t mmio_read_32(void *address) {
    return *(uint32_t *) address;
}

static inline uint16_t mmio_read_16(void *address) {
    return *(uint16_t *) address;
}

static inline uint8_t mmio_read_8(void *address) {
    return *(uint8_t *) address;
}

//
//
//
//
//
// =============================| MISC |==============================
//
//
//
//
//

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static inline void cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd) {
    __asm__ volatile("cpuid"
                     : "=a"(abcd[0]), "=b"(abcd[1]), "=c"(abcd[2]),
                       "=d"(abcd[3])
                     : "a"(eax), "c"(ecx));
}

static inline uint64_t read_cr4() {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void write_cr4(uint64_t cr4) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

static inline uint32_t get_core_id(void) {
    uint32_t eax, ebx, ecx, edx;

    eax = 1;
    __asm__ volatile("cpuid"
                     : "=b"(ebx), "=a"(eax), "=c"(ecx), "=d"(edx)
                     : "a"(eax));

    return (ebx >> 24) & 0xFF;
}

static inline bool are_interrupts_enabled() {
    unsigned long flags;
    __asm__ volatile("pushf\n\t"
                     "pop %0\n\t"
                     : "=r"(flags)
                     :
                     :);
    return (flags & (1 << 9)) != 0;
}

#define MSR_GS_BASE 0xC0000101

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = value & 0xFFFFFFFF;
    uint32_t hi = value >> 32;
    asm volatile("wrmsr" ::"c"(msr), "a"(lo), "d"(hi) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint64_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (hi << 32U) | lo;
}

uint64_t get_sch_core_id();

#pragma once
