/* @title: Console report composer */
#pragma once
#include <compiler.h>
#include <console/term.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compose a report as a stream of lines, all statically allocated
 * with no locks taken, primarily useful for panicking */

/* Bytes for a composed line. Full width rules of box drawing glyphs cost
 * three bytes a column, so we use this to bound terminal width */
#define REPORT_LINE_MAX 1024

/* Build a line into a caller supplied buffer, counting display columns
 *
 * `max_col` bounds layout and `cap` bounds buffer, and partial writes
 * are not allowed, and overflow uses ellipsis */
struct report_line {
    char *buf;
    size_t cap;
    size_t len;      /* bytes used */
    size_t col;      /* display columns used */
    size_t max_col;  /* display column bound/budget */
    size_t glyph_at; /* byte offset of the last visible glyph */
    bool full;       /* budget exhausted */
    bool truncated;  /* something was cut somewhere on this line */
};

#define REPORT_LINE(name, width)                                               \
    char name##_storage[REPORT_LINE_MAX];                                      \
    struct report_line name;                                                   \
    report_line_init(&name, name##_storage, sizeof(name##_storage), (width))

void report_line_init(struct report_line *l, char *buf, size_t cap,
                      size_t max_col);
void report_line_reset(struct report_line *l);

void report_line_puts(struct report_line *l, const char *s);
__printf_like(2, 3) void report_line_printf(struct report_line *l,
                                            const char *fmt, ...);
void report_line_vprintf(struct report_line *l, const char *fmt, va_list ap);

/* Spaces out to a display column */
void report_line_pad_to(struct report_line *l, size_t col);
void report_line_repeat(struct report_line *l, const char *glyph, size_t n);

/* take up exactly `width` display columns, and truncate if too long/pad
 * if too short */
void report_line_field(struct report_line *l, const char *s, size_t width);
void report_line_right(struct report_line *l, const char *s);

const char *report_line_str(const struct report_line *l);
size_t report_line_width(const struct report_line *l);
bool report_line_truncated(const struct report_line *l);

/* Display columns a string would occupy */
size_t report_strwidth(const char *s);

struct report_panes;
struct report_box;

/* Where a finished line actually goes and how wide it is, either the
 * console or the pane of a buffered region
 *
 * `col` means a display column, a pane is the vertical panel */
struct report_target {
    struct report_panes *panes; /* NULL means the console/a box */
    struct report_box *box;     /* non-NULL = lines come out bordered */
    uint16_t width;             /* display columns */
    uint8_t indent;
    uint8_t pane; /* which pane of `panes` */
};

struct report_target report_console(void);
struct report_target report_console_indent(uint16_t indent);
struct report_target report_pane(struct report_panes *panes, uint32_t pane);

/* Nesting indentation supported */
struct report_target report_target_indent(struct report_target tgt,
                                          uint16_t indent);

static inline uint16_t report_target_width(const struct report_target *tgt) {
    return tgt->width;
}

/* Emit a line, don't pass '\n' */
void report_puts(struct report_target *tgt, const char *s);
__printf_like(2, 3) void report_printf(struct report_target *tgt,
                                       const char *fmt, ...);
void report_blank(struct report_target *tgt);

void report_line_emit(struct report_target *tgt, struct report_line *l);

/* --- TITLE ---- , to the target's width */
void report_rule(struct report_target *tgt, const char *title);
void report_rule_sev(struct report_target *tgt, enum term_sev sev,
                     const char *title);

/* Word wrap to `tgt`'s width, emitting however many lines it takes
 *
 * Break at spaces, honor newlines, count display columns */
void report_wrap(struct report_target *tgt, const char *text);
__printf_like(2, 3) void report_wrap_printf(struct report_target *tgt,
                                            const char *fmt, ...);

/* A box, such as
 *
 *   ┌─ title ─────────────┐
 *   │ this is my box data │
 *   └─────────────────────┘
 *
 */
struct report_box {
    struct report_target target;
    uint16_t inner; /* content columns, i.e.
                     * everything but borders and padding */
};

/* inner == 0 fills target, size with `report_strwidth()`, when the box
 * should hug rather than span the terminal, content wraps */
void report_box_open(struct report_box *b, struct report_target target,
                     const char *title, uint16_t inner);

/* This wraps, different from all the other _printf functions here */
__printf_like(2, 3) void report_box_printf(struct report_box *b,
                                           const char *fmt, ...);

/* A target whose lines get bordered, so anything that draws
 * to a target works in a box, its width is the box's inner width */
struct report_target report_box_body(struct report_box *b);
void report_box_close(struct report_box *b);

/* Gives a header to a region we're inside of it, with faults being recorded
 * to avoid nested panics breaking the report */
bool report_section_begin(const char *name);
bool report_section_begin_at(struct report_target *tgt, const char *name);

/* No heading emitted, for callers that already have drawn one */
bool report_section_claim(const char *name);
bool report_section_claim_at(struct report_target *tgt, const char *name);
void report_section_end(void);
const char *report_section_current(void);

#define REPORT_FIELD_GAP 2

/* packs kv pairs, as many per row as target is wide, flushing as it fills,
 * holding a line writer pointing into its storage */
struct report_fields {
    struct report_target target;
    uint8_t per_row;
    uint8_t in_row;
    uint8_t keyw;
    uint8_t valw;
    struct report_line line;
    char storage[REPORT_LINE_MAX];
};

void report_fields_begin(struct report_fields *g, struct report_target target,
                         uint32_t keyw, uint32_t valw);
__printf_like(3, 4) void report_field(struct report_fields *g, const char *key,
                                      const char *fmt, ...);

/* End row and take the full line, for values carrying annotations too long */
__printf_like(3, 4) void report_field_full(struct report_fields *g,
                                           const char *key, const char *fmt,
                                           ...);
void report_fields_end(struct report_fields *g);

/* Storage is MAX * LINES * BYTES */
#define REPORT_PANES_MAX 3

/* Max rows for a pane */
#define REPORT_PANE_ROWS 48

/* Bytes stored per line */
#define REPORT_PANE_LINE_MAX 384
#define REPORT_PANE_MIN_WIDTH 24
#define REPORT_PANE_GAP 3

/* Vertical panels side by side, only 2D thing here, and the only buffered one
 *
 * Panes can't stream, and lines are stored per-pane and interleaved at flush
 *
 * This is statically allocated */
struct report_panes {
    uint8_t n;
    uint16_t width[REPORT_PANES_MAX];
    const char *title[REPORT_PANES_MAX];
    uint8_t title_sev[REPORT_PANES_MAX];
    uint16_t nrows[REPORT_PANES_MAX];
    uint32_t dropped[REPORT_PANES_MAX];
    bool stacked;   /* too narrow to sit side by side */
    bool undivided; /* no vertical rule */
    char text[REPORT_PANES_MAX][REPORT_PANE_ROWS][REPORT_PANE_LINE_MAX];

    /* Which rows are rules */
    bool rule[REPORT_PANES_MAX][REPORT_PANE_ROWS];
};

/* `weights` == NULL is even split */
void report_panes_begin(struct report_panes *panes, uint32_t n,
                        const uint8_t *weights);

/* Remove the vertical rule between panes, leaving only the gap */
void report_panes_undivided(struct report_panes *panes);

/* Heading for a pane, drawn as a segment of the pane's opening rule */
void report_panes_title(struct report_panes *panes, uint32_t pane,
                        enum term_sev sev, const char *title);

/* Framing rules with dividers:
 *
 *   -- text -----+--- more ------
 *   ...
 *   -- data -----+---------------
 *
 * The opening rule is segmented, and closing rule can double as a title
 */
void report_panes_top(struct report_panes *panes);
void report_panes_bottom(struct report_panes *panes, enum term_sev sev,
                         const char *title);

/* Hand this region's divider positions to next opening rule, so one
 * rule can close the region above and open one below, such as
 *
 * -----------+------
 *
 * -- text ---+------
 *
 * Call after flushing and before `begin()` changes widths, consumed by
 * the next `report_panes_top()` */
void report_panes_carry(struct report_panes *panes);

/* Below REPORT_PANE_MIN_WIDTH, the panes stack vertically */
void report_panes_flush(struct report_panes *panes);
uint16_t report_pane_rows(const struct report_panes *panes, uint32_t pane);
uint16_t report_panes_rows_max(const struct report_panes *panes);

/* Reserved for panic paths, so callers don't need to own the storage, regions
 * get used one at a time with `begin()` resetting it */
struct report_panes *report_panes_panic(void);

/* Enter panic report path, so writes are lock free and the screen
 * is given back, used in panic handler and tracks nested panics */
void report_enter_panic(void);
