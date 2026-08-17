#pragma once
#include <stdint.h>
#include <types/types.h>

#define PIT_FREQUENCY 1193182
freq_hz_t measure_tsc_freq_pit(void);
