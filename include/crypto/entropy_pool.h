#include <stdint.h>
#include <spin_lock.h>
#pragma once
#define ENTROPY_POOL_SIZE 64  // 512 bits
#define ENTROPY_MAX_BITS (ENTROPY_POOL_SIZE * 8)

struct entropy_pool {
    uint8_t buffer[ENTROPY_POOL_SIZE];  // Raw pool data
    uint64_t write_pos;                   // Circular buffer write position
    uint64_t entropy_bits;                // Estimated entropy in bits
    struct spinlock lock;                    // To protect concurrent access
};

