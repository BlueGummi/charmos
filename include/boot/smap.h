#pragma once
#include <stdint.h>
void cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd);
void smap_init();
