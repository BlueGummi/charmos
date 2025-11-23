/* @title: PRNG */
#pragma once
#include <stdint.h>
void prng_seed(uint64_t seed);
uint64_t prng_next();
