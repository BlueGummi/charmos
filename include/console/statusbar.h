/* @title: Pinned status bar */
#pragma once
#include <compiler.h>
#include <console/report.h>
#include <console/term.h>
#include <stdarg.h>
#include <stddef.h>
#include <time/time.h>

/* This is the only place that moves the cursor. DECSTBM (CSI top;bottom r)
 * shrinks the region that scrolls, and CUP (CSI row; col H) addresses the row
 * left outside it, so the bar stays pinned while log output scrolls above it.
 *
 * We borrow the line writer for report.h to build the text */
void status_bar_open(void);
void status_bar_close(void);
__printf_like(1, 2) void status_bar_set(const char *fmt, ...);

/* [####----] 34/120 (28%) <detail...>, painted onto the bar */
__printf_like(3, 4) void status_bar_progress(size_t done, size_t total,
                                             const char *detail_fmt, ...);

__printf_like(4, 5) void status_bar_progress_timed(size_t done, size_t total,
                                                   time_ms_t started_ms,
                                                   const char *detail_fmt, ...);

/* Hand the whole screen back: drop the scroll region, forget the bar */
void status_bar_reset(void);
