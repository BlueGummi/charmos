/* @title: Address sanitization */
#include <stddef.h>

#define ASAN_SHADOW_SCALE 3ULL /* 1 shadow byte per 8 real bytes */
#define ASAN_SHADOW_OFFSET                                                     \
    0xfffffc0000000000ULL /* fixed virtual base for shadow memory */
#define ASAN_REDZONE 16   /* optional redzone per allocation */
#define ASAN_POISON_VALUE 0xFF
#define ASAN_ABORT_IF_NOT_READY()                                              \
    do {                                                                       \
        if (!asan_ready)                                                       \
            return;                                                            \
    } while (0)

void asan_init(void);
void asan_poison(void *addr, size_t size);
void asan_unpoison(void *addr, size_t size);
