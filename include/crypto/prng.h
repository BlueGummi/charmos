/* @title: PRNG */
#pragma once
#include <stdint.h>

#define PRNG_SPLITMIX64_GAMMA UINT64_C(0x9e3779b97f4a7c15)
#define PRNG_SPLITMIX64_M1 UINT64_C(0xbf58476d1ce4e5b9)
#define PRNG_SPLITMIX64_M2 UINT64_C(0x94d049bb133111eb)

static inline uint64_t prng_splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += PRNG_SPLITMIX64_GAMMA);
    z = (z ^ (z >> 30)) * PRNG_SPLITMIX64_M1;
    z = (z ^ (z >> 27)) * PRNG_SPLITMIX64_M2;
    return z ^ (z >> 31);
}

void prng_seed(uint64_t seed);
uint64_t prng_next(void);
