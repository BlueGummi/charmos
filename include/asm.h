/* @title: Assembly Routines */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <stdbool.h>
#include <stdint.h>
#include <tsa.h>

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
// =============================| MISC |==============================
//
//
//
//
//

static inline void cr8_write(uint64_t cr8) {
    asm volatile("mov %0, %%cr8" ::"r"(cr8));
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static inline uint64_t rdtsc_ordered(void) {
    uint32_t lo, hi;
    asm volatile("lfence\n\t"
                 "rdtsc"
                 : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static inline void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                               uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

static inline uint64_t cr0_read(void) {
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void cr0_write(uint64_t cr0) {
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static inline uint64_t cr4_read(void) {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void cr4_write(uint64_t cr4) {
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
}

static inline bool irqs_enabled() {
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

static inline void io_port_wait(void) {
    outb(0x80, 0);
}

TSA_REENTRANT_CAPABILITY_DECLARE("irqs", IRQS_DISABLED);

static inline void irq_enable(void) TSA_RELEASES_IRQS TSA_NO_ANALYSIS {
    asm volatile("sti");
}

static inline void irq_disable(void) TSA_ACQUIRES_IRQS TSA_NO_ANALYSIS {
    asm volatile("cli");
}

static inline void irq_enable_untracked(void) {
    asm volatile("sti");
}

static inline bool irq_disable_save(void) TSA_ACQUIRES_IRQS TSA_NO_ANALYSIS {
    bool enabled = irqs_enabled();
    irq_disable();
    return enabled;
}

static inline void
irq_restore(bool was_enabled) TSA_RELEASES_IRQS TSA_NO_ANALYSIS {
    if (was_enabled)
        irq_enable();
}

static inline cc_always_inline cc_maybe_unused void
irq_save_guard_exit(bool *was_enabled) TSA_RELEASES_IRQS TSA_NO_ANALYSIS {
    irq_restore(*was_enabled);
}

#define irq_save_guard()                                                       \
    cc_cleanup(irq_save_guard_exit) bool PP_CONCAT(                            \
        irq_save_guard_, __COUNTER__) = irq_disable_save()

#define irq_disable_guard() irq_save_guard()

static inline void tlb_invlpg(uint64_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline uint64_t cr2_read(void) {
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static inline uint64_t cr3_read(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void cr3_write(uint64_t cr3) {
    asm volatile("mov %0, %%cr3" ::"r"(cr3));
}

static inline void tlb_flush(void) {
    uint64_t cr3 = cr3_read();
    cr3_write(cr3);
}

static inline void cpu_pause(void) {
    asm volatile("pause");
}

static inline void cpu_halt(void) {
    asm volatile("hlt");
}

static inline void cpu_freeze(void) {
    asm volatile("cli; hlt");
}

/* sti; hlt: ends the caller's interrupts-off region */
static inline void cpu_idle(void) TSA_RELEASES_IRQS TSA_NO_ANALYSIS {
    asm volatile("sti\n\thlt" ::: "memory");
}

static inline int bit_clz8(uint8_t a) {
    return ci_clz(a);
}

static inline void cpu_memory_barrier(void) {
    asm volatile("mfence" ::: "memory");
}

static inline void cpu_full_fence(void) {
    asm volatile("mfence; lfence" ::: "memory");
}

static inline uint64_t rdtsc_after_stores(void) {
    uint32_t lo, hi;
    asm volatile("mfence; lfence; rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t) hi << 32) | lo;
}

static inline uint64_t rdtsc_before_loads(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc; lfence" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t) hi << 32) | lo;
}
