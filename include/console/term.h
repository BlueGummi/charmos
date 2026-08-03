/* @title: Console terminal state */
#pragma once
#include <compiler.h>
#include <stdbool.h>
#include <stdint.h>

#define TERM_PROBE_TIMEOUT_MS 250

#define TERM_FALLBACK_ROWS 25
#define TERM_FALLBACK_COLS 80

struct term_geometry {
    uint16_t rows;
    uint16_t cols;
};

/* Ask terminal about size */
bool term_probe(void);
bool term_available(void);

/* falls back to FALLBACK */
struct term_geometry term_size(void);

/* Pin the geometry */
void term_geometry_set(uint16_t rows, uint16_t cols);

/* Plain mode strips escapes, swapping box drawing for ASCII */
void term_set_plain(bool plain);
bool term_plain(void);

void term_set_unicode(bool unicode);
bool term_unicode(void);

enum term_sev {
    TERM_SEV_NORMAL,
    TERM_SEV_DIM,
    TERM_SEV_LABEL,
    TERM_SEV_OK,
    TERM_SEV_WARN,
    TERM_SEV_CRIT,
    TERM_SEV_HEAD,
};

/* Severities instead of raw ANSI so plain mode just removes color here */
const char *term_style(enum term_sev sev);
const char *term_style_reset(void);

/* Mark console broken, drops scroll region, and locks don't get taken */
void term_enter_panic(void);
bool term_in_panic(void);
