#include <stddef.h>
void asan_init(void);
void asan_poison(void *addr, size_t size);
void asan_unpoison(void *addr, size_t size);
