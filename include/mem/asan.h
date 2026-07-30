/* @title: Address sanitization */
#include <errno.h>
#include <log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

#define ASAN_SHADOW_SCALE 3ULL /* 1 shadow byte per 8 real bytes */
#define ASAN_SHADOW_OFFSET 0xDFFFFE0000000000

/*
 * offset = SHADOW_REGION_START - (COVERED_START >> SHADOW_SCALE)
 * shadow = (addr >> scale) + offset */
#define ASAN_SHADOW_ADDR(a)                                                    \
    ((uintptr_t) (((uintptr_t) (a) >> ASAN_SHADOW_SCALE) + ASAN_SHADOW_OFFSET))
#define ASAN_GRANULE (1ULL << ASAN_SHADOW_SCALE)
#define ASAN_REDZONE 16 /* optional redzone per allocation */

/* Shadow byte encoding, one per ASAN_GRANULE bytes of memory:
 *
 * 0 - whole granule accessible
 * 1..7 first k bytes accessible
 * >= 0x80 nothing in granule accessible */
#define ASAN_POISON_VALUE 0xFF        /* generic: never handed out */
#define ASAN_POISON_HEAP_REDZONE 0xFA /* slack around/inside a live slot */
#define ASAN_POISON_HEAP_FREED 0xFD   /* was live, has been freed */

static inline bool asan_shadow_is_poison(uint8_t s) {
    return s >= 0x80;
}

#define ASAN_ABORT_IF_NOT_READY()                                              \
    do {                                                                       \
        if (!asan_ready)                                                       \
            return;                                                            \
    } while (0)

LOG_SITE_EXTERN(asan);
LOG_HANDLE_EXTERN(asan);

#define asan_log(lvl, fmt, ...)                                                \
    log(LOG_SITE(asan), LOG_HANDLE(asan), lvl, fmt, ##__VA_ARGS__)

#define asan_err(fmt, ...) asan_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define asan_warn(fmt, ...) asan_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define asan_info(fmt, ...) asan_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define asan_debug(fmt, ...) asan_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define asan_trace(fmt, ...) asan_log(LOG_TRACE, fmt, ##__VA_ARGS__)

#ifdef DEBUG_ASAN

void asan_init(void);

/* Shadow backing at the slab chunk granularity, which must happen before any
 * object in the chunk is handed off */
enum errno asan_shadow_install(vaddr_t base, size_t len);
void asan_shadow_release(vaddr_t base, size_t len);

void asan_alloc(void *addr, size_t requested, size_t slot);
void asan_free(void *addr, size_t slot);
void asan_poison(void *addr, size_t size);
void asan_unpoison(void *addr, size_t size);

#else

static inline void asan_init(void) {}

static inline enum errno asan_shadow_install(vaddr_t base, size_t len) {
    (void) base;
    (void) len;
    return ERR_OK;
}

static inline void asan_shadow_release(vaddr_t base, size_t len) {
    (void) base;
    (void) len;
}

static inline void asan_alloc(void *addr, size_t requested, size_t slot) {
    (void) addr;
    (void) requested;
    (void) slot;
}

static inline void asan_free(void *addr, size_t slot) {
    (void) addr;
    (void) slot;
}

static inline void asan_poison(void *addr, size_t size) {
    (void) addr;
    (void) size;
}

static inline void asan_unpoison(void *addr, size_t size) {
    (void) addr;
    (void) size;
}

#endif
