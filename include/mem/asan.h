/* @title: Address sanitization */
#include <log.h>
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

LOG_SITE_EXTERN(asan);
LOG_HANDLE_EXTERN(asan);

#define asan_log(lvl, fmt, ...)                                                \
    log(LOG_SITE(asan), LOG_HANDLE(asan), lvl, fmt, ##__VA_ARGS__)

#define asan_err(fmt, ...) asan_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define asan_warn(fmt, ...) asan_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define asan_info(fmt, ...) asan_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define asan_debug(fmt, ...) asan_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define asan_trace(fmt, ...) asan_log(LOG_TRACE, fmt, ##__VA_ARGS__)

void asan_init(void);
void asan_poison(void *addr, size_t size);
void asan_unpoison(void *addr, size_t size);
