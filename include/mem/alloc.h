#include <stdint.h>

void *kmalloc(uint64_t size);
void *krealloc(void *ptr, uint64_t size);
void *kzalloc(uint64_t size);
void kfree(void *ptr);
#pragma once
