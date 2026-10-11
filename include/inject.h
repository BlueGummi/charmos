/* @title: Code Fuzzing Injections */
#pragma once
#include <atomic.h>
#include <linker/symbols.h>
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

    atomic_bool armed;
    atomic_uint32_t seed;
    atomic_uint32_t nth;
    atomic_uint32_t counter;
};

LINKER_SECTION_EXTERN(struct inject_site, inject_sites);

#define INJECT_SITE_ATTRIBUTE cc_section(".kernel_inject_sites") cc_used

#define INJECT_SITE_DEFINE(id, injkind, description)                           \
    INJECT_SITE_ATTRIBUTE struct inject_site __inject_site_##id = {            \
        .name = #id, .desc = (description), .kind = (injkind)}

#define INJECT_SITE_EXTERN(id) extern struct inject_site __inject_site_##id
#define INJECT_SITE(id) (&__inject_site_##id)

static inline void inject_arm(struct inject_site *s, uint32_t seed,
                              uint32_t nth) {
    atomic_store_rlx(&s->seed, seed);
    atomic_store_rlx(&s->nth, nth);
    atomic_store_rlx(&s->counter, 0);
    atomic_store_rlx(&s->armed, true);
}

static inline void inject_disarm(struct inject_site *s) {
    atomic_store_rlx(&s->armed, false);
}

#ifdef INJECT_ENABLED
void inject_delay_impl(struct inject_site *s);
cc_warn_unused_result bool inject_fail_impl(struct inject_site *s);

#define INJECT_DELAY(id)                                                       \
    do {                                                                       \
        if (cc_unlikely(atomic_load_rlx(&INJECT_SITE(id)->armed)))             \
            inject_delay_impl(INJECT_SITE(id));                                \
    } while (0)

#define INJECT_FAIL(id)                                                        \
    (cc_unlikely(atomic_load_rlx(&INJECT_SITE(id)->armed)) &&                  \
     inject_fail_impl(INJECT_SITE(id)))
#else
#define INJECT_DELAY(id) ((void) 0)
#define INJECT_FAIL(id) (false)
#endif
