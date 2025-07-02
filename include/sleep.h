#include <stdbool.h>
#include <stdint.h>
void sleep(uint64_t seconds);
void sleep_us(uint64_t us);
void sleep_ms(uint64_t msec);
bool mmio_wait(uint32_t *reg, uint32_t mask, uint64_t timeout);
#pragma once
