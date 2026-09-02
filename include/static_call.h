/* @title: Static Call */
#pragma once
#include <rw_once.h>
#include <stdint.h>
#include <text_patch.h>

#define STATIC_CALL_DECLARE(name, default_fn)                                  \
    extern __typeof__(default_fn) name##_trampoline;                           \
    asm(".pushsection .text, \"ax\"\n\t"                                       \
        ".globl " #name "_trampoline\n\t" #name "_trampoline:\n\t"             \
        ".byte 0xe9\n\t" /* jmp rel32 */                                       \
        ".long " #default_fn " - (" #name "_trampoline + 5)\n\t"               \
        ".popsection\n\t")

#define static_call(name) name##_trampoline

static inline void __static_call_update(void *trampoline, void *fn) {
    uint8_t *p = trampoline;
    uint32_t rel = (uint32_t) ((uintptr_t) fn - (uintptr_t) trampoline - 5);

    struct text_patch_window window = text_patch_begin();
    WRITE_ONCE(*(uint32_t *) (p + 1), rel);
    text_patch_end(window);
}

#define static_call_update(name, fn)                                           \
    do {                                                                       \
        __typeof__(&name##_trampoline) _scu_fn = (fn);                         \
        __static_call_update((void *) name##_trampoline, (void *) _scu_fn);    \
    } while (0)
