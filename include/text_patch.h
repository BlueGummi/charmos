/* @title: Kernel Text Patching */
#pragma once
#include <asm.h>
#include <stdbool.h>
#include <stdint.h>

#define CR0_WP (1UL << 16)

struct text_patch_window {
    uint64_t cr0;
    bool interrupts;
};

static inline struct text_patch_window text_patch_begin(void) {
    struct text_patch_window w = {
        .interrupts = are_interrupts_enabled(),
    };

    disable_interrupts();
    w.cr0 = read_cr0();
    write_cr0(w.cr0 & ~CR0_WP);

    return w;
}

static inline void text_patch_end(struct text_patch_window w) {
    write_cr0(w.cr0);

    if (w.interrupts)
        enable_interrupts();
}
