#include <stdbool.h>
#include <stdint.h>
void sleep_spin(uint64_t seconds);
void sleep_spin_us(uint64_t us);
void sleep_spin_ms(uint64_t msec);
bool mmio_spin_wait(uint32_t *reg, uint32_t mask, uint64_t timeout);
#pragma once
