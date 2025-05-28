#include <stddef.h>

void slab_init();
void *kmalloc(size_t size);
void kfree(void *ptr);
#pragma once
