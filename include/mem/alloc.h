#include <stddef.h>

void *kmalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);
#pragma once
