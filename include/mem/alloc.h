#include <stdint.h>

void *kmalloc(uint64_t size);
void *krealloc(void *ptr, uint64_t size);
void *kzalloc(uint64_t size);
void kfree(void *ptr);
void *kmalloc_aligned(uint64_t size, uint64_t align);
void kfree_aligned(void *ptr);
#pragma once
