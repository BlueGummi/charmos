#include <console/printf.h>
#include <mp/core.h>
#include <stdbool.h>
#include <stdint.h>

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
// ========================| MMIO  |=========================
//
//
//
//
//

static inline void mmio_write_64(void *address, uint64_t value) {
    asm volatile("movq %0, (%1)" : : "r"(value), "r"(address) : "memory");
}

static inline void mmio_write_32(void *address, uint32_t value) {
    asm volatile("movl %0, (%1)" : : "r"(value), "r"(address) : "memory");
}

static inline void mmio_write_16(void *address, uint16_t value) {
    asm volatile("movw %0, (%1)" : : "r"(value), "r"(address) : "memory");
}

static inline void mmio_write_8(void *address, uint8_t value) {
    asm volatile("movb %0, (%1)" : : "r"(value), "r"(address) : "memory");
}

static inline uint64_t mmio_read_64(void *address) {
    uint64_t value;
    asm volatile("movq (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

static inline uint32_t mmio_read_32(void *address) {
    uint32_t value;
    asm volatile("movl (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

static inline uint16_t mmio_read_16(void *address) {
    uint16_t value;
    asm volatile("movw (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

static inline uint8_t mmio_read_8(void *address) {
    uint8_t value;
    asm volatile("movb (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
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
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static inline void cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd) {
    asm volatile("cpuid"
                 : "=a"(abcd[0]), "=b"(abcd[1]), "=c"(abcd[2]), "=d"(abcd[3])
                 : "a"(eax), "c"(ecx));
}

static inline uint64_t read_cr4() {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void write_cr4(uint64_t cr4) {
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
}

static inline uint32_t get_core_id(void) {
    uint32_t eax, ebx, ecx, edx;

    eax = 1;
    asm volatile("cpuid"
                 : "=b"(ebx), "=a"(eax), "=c"(ecx), "=d"(edx)
                 : "a"(eax));

    return (ebx >> 24) & 0xFF;
}

static inline bool are_interrupts_enabled() {
    unsigned long flags;
    asm volatile("pushf\n\t"
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

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline uint64_t get_sch_core_id() {
    uint64_t id;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(id)
                 : "i"(offsetof(struct core, id)));
    return id;
}

static inline void clear_interrupts(void) {
    asm volatile("cli");
}

static inline void restore_interrupts(void) {
    asm volatile("sti");
}

static inline void invlpg(uint64_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void cpu_relax(void) {
    asm volatile("pause");
}

#pragma once
