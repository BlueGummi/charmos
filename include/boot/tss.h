#pragma once
#include <stdint.h>

struct __attribute__((aligned)) __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp0; // Stack pointer for ring 0
    uint64_t rsp1; // Stack pointer for ring 1 (optional)
    uint64_t rsp2; // Stack pointer for ring 2 (optional)
    uint64_t reserved1;
    uint64_t ist1; // Interrupt Stack Table entry 1
    uint64_t ist2; // IST2
    uint64_t ist3; // IST3
    uint64_t ist4; // IST4
    uint64_t ist5; // IST5
    uint64_t ist6; // IST6
    uint64_t ist7; // IST7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base; // Offset to I/O permission bitmap
};
