/* @title: Code Fuzzing Injections */
#pragma once
#include <linker/symbols.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum inject_kind {
    INJECT_KIND_DELAY = 1,     /* spin */
    INJECT_KIND_FAIL = 1 << 1, /* Force failures */
};

struct inject_site {
    const char *name;
    const char *desc;
    enum inject_kind kind;

    _Atomic bool armed;
    _Atomic uint32_t seed;
    _Atomic uint32_t nth;
    _Atomic uint32_t counter;
};

LINKER_SECTION_DEFINE(struct inject_site, inject_sites);

/* Have to do this as it is not static */
#define INJECT_SITE_ATTRIBUTE                                                  \
    __attribute__((section(".kernel_inject_sites"), used))

#define INJECT_SITE_DECLARE(id, injkind, description)                          \
    INJECT_SITE_ATTRIBUTE struct inject_site __inject_site_##id = {            \
        .name = #id, .desc = (description), .kind = (injkind)}

#define INJECT_SITE_DEFINE(id) extern struct inject_site __inject_site_##id
#define INJECT_SITE(id) (&__inject_site_##id)

static inline void inject_arm(struct inject_site *s, uint32_t seed,
                              uint32_t nth) {
    atomic_store_explicit(&s->seed, seed, memory_order_relaxed);
    atomic_store_explicit(&s->nth, nth, memory_order_relaxed);
    atomic_store_explicit(&s->counter, 0, memory_order_relaxed);
    atomic_store_explicit(&s->armed, true, memory_order_relaxed);
}

static inline void inject_disarm(struct inject_site *s) {
    atomic_store_explicit(&s->armed, false, memory_order_relaxed);
}

#ifdef INJECT_ENABLED
void inject_delay_impl(struct inject_site *s);
bool inject_fail_impl(struct inject_site *s) __warn_unused_result;

#define INJECT_DELAY(id)                                                       \
    do {                                                                       \
        if (unlikely(atomic_load_explicit(&INJECT_SITE(id)->armed,             \
                                          memory_order_relaxed)))              \
            inject_delay_impl(INJECT_SITE(id));                                \
    } while (0)

#define INJECT_FAIL(id)                                                        \
    (unlikely(atomic_load_explicit(&INJECT_SITE(id)->armed,                    \
                                   memory_order_relaxed)) &&                   \
     inject_fail_impl(INJECT_SITE(id)))
#else
#define INJECT_DELAY(id) ((void) 0)
#define INJECT_FAIL(id) (false)
#endif
