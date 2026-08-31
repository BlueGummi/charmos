/* @title: Console report composer */
#include <asm.h>
#include <colors.h>
#include <compiler.h>
#include <console/printf.h>
#include <console/report.h>
#include <console/term.h>
#include <sch/irql.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Which section is being rendered right now, so a panic arriving mid render
 * can state which is being faulted and skip on the way through */
static const char *report_section;
static const char *report_skip_section;

static struct report_panes panic_panes;

static const char *glyph_hbar(void) {
    return term_unicode() && !term_plain() ? "─" : "-";
}

static const char *glyph_vbar(void) {
    return term_unicode() && !term_plain() ? "│" : "|";
}

static const char *glyph_ellipsis(void) {
    return term_unicode() && !term_plain() ? "…" : ">";
}

static const char *glyph_join(bool left, bool right) {
    bool uni = term_unicode() && !term_plain();

    if (left && right)
        return uni ? "┼" : "+";
    if (left)
        return uni ? "┤" : "+";
    if (right)
        return uni ? "├" : "+";

    return uni ? "│" : "|";
}

static const char *glyph_tee_up(void) {
    return term_unicode() && !term_plain() ? "┴" : "+";
}

/* A divider meets a horizontal rule */
static const char *glyph_tee(bool above, bool below) {
    bool uni = term_unicode() && !term_plain();

    if (above && below)
        return uni ? "┼" : "+";
    if (above)
        return uni ? "┴" : "+";
    if (below)
        return uni ? "┬" : "+";

    return uni ? "─" : "-";
}

static const char *glyph_continue(void) {
    return term_unicode() && !term_plain() ? "↩" : "\\";
}

void report_line_init(struct report_line *l, char *buf, size_t cap,
                      size_t max_col) {
    l->buf = buf;
    l->cap = cap;
    l->max_col = max_col;

    if (buf && cap)
        buf[0] = '\0';

    report_line_reset(l);
}

void report_line_reset(struct report_line *l) {
    l->len = 0;
    l->col = 0;
    l->glyph_at = 0;
    l->full = false;
    l->truncated = false;

    if (l->buf && l->cap)
        l->buf[0] = '\0';
}

/* Callers hand one escape sequence or a whole glyph at a time,
 * so refusing partial writes keep byte caps from landing in the middle...
 *
 * Half a UTF-8 glyph prints as a [?] and half a CSI eats the next
 * thing the terminal reads */
static bool line_raw(struct report_line *l, const char *s, size_t n) {
    if (!l->buf || l->cap == 0 || l->len + n >= l->cap)
        return false;

    memcpy(l->buf + l->len, s, n);
    l->len += n;
    l->buf[l->len] = '\0';
    return true;
}

static void line_mark_full(struct report_line *l) {
    const char *ell = glyph_ellipsis();

    l->full = true;
    l->truncated = true;

    if (l->col == 0)
        return;

    l->len = l->glyph_at;
    l->buf[l->len] = '\0';
    l->col--;

    if (line_raw(l, ell, strlen(ell)))
        l->col++;
}

/* Past the end of an escape sequence starting at `s` */
static const char *esc_skip(const char *s, const char *end) {
    s++;

    if (s < end && *s == '[') {
        s++;

        while (s < end && (*s < '@' || *s > '~'))
            s++;
    }

    if (s < end)
        s++;

    return s;
}

static void line_puts_n(struct report_line *l, const char *s, const char *end) {
    if (l->full)
        return;

    while (s < end) {
        if (*s == '\033') {
            const char *start = s;

            s = esc_skip(s, end);

            /* Plain mode strips here, so color format strings need no changes,
             * and a styling escape that will not fit is dropped */
            if (!term_plain())
                line_raw(l, start, (size_t) (s - start));

            continue;
        }

        if ((unsigned char) *s < 0x20 || *s == 0x7f) {
            s++;
            continue;
        }

        if (l->col >= l->max_col) {
            line_mark_full(l);
            return;
        }

        /* Lead byte plus continuations */
        const char *glyph = s++;

        while (s < end && (*s & 0xC0) == 0x80)
            s++;

        l->glyph_at = l->len;

        if (!line_raw(l, glyph, (size_t) (s - glyph))) {
            line_mark_full(l);
            return;
        }

        l->col++;
    }
}

void report_line_puts(struct report_line *l, const char *s) {
    if (!s)
        return;

    line_puts_n(l, s, s + strlen(s));
}

void report_line_vprintf(struct report_line *l, const char *fmt, va_list ap) {
    char buf[REPORT_LINE_MAX];

    if (l->full)
        return;

    vsnprintf(buf, (int) sizeof(buf), fmt, ap);
    report_line_puts(l, buf);
}

void report_line_printf(struct report_line *l, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    report_line_vprintf(l, fmt, ap);
    va_end(ap);
}

void report_line_pad_to(struct report_line *l, size_t col) {
    if (col > l->max_col)
        col = l->max_col;

    while (l->col < col) {
        if (!line_raw(l, " ", 1))
            break;

        l->col++;
    }
}

void report_line_repeat(struct report_line *l, const char *glyph, size_t n) {
    for (size_t i = 0; i < n && !l->full; i++)
        report_line_puts(l, glyph);
}

void report_line_field(struct report_line *l, const char *s, size_t width) {
    if (l->full)
        return;

    size_t outer = l->max_col;
    size_t end = l->col + width;

    if (end > outer)
        end = outer;

    l->max_col = end;
    report_line_puts(l, s);
    l->full = false;
    report_line_pad_to(l, end);

    l->max_col = outer;
    l->full = l->col >= outer;
}

void report_line_right(struct report_line *l, const char *s) {
    size_t w = report_strwidth(s);

    if (l->col + w < l->max_col)
        report_line_pad_to(l, l->max_col - w);

    report_line_puts(l, s);
}

const char *report_line_str(const struct report_line *l) {
    return l->buf ? l->buf : "";
}

size_t report_line_width(const struct report_line *l) {
    return l->col;
}

bool report_line_truncated(const struct report_line *l) {
    return l->truncated;
}

size_t report_strwidth(const char *s) {
    size_t w = 0;

    if (!s)
        return 0;

    while (*s) {
        if (*s == '\033') {
            s++;

            if (*s == '[') {
                s++;
                while (*s && (*s < '@' || *s > '~'))
                    s++;
            }

            if (*s)
                s++;

            continue;
        }

        if ((unsigned char) *s < 0x20 || *s == 0x7f) {
            s++;
            continue;
        }

        if ((*s & 0xC0) != 0x80)
            w++;

        s++;
    }

    return w;
}

struct report_guard {
    enum irql irql;
    bool locked;
};

static struct report_guard report_lock(void) {
    struct report_guard g = {.irql = 0, .locked = false};

    if (term_in_panic())
        return g;

    g.irql = printf_lock();
    g.locked = true;
    return g;
}

static void report_unlock(struct report_guard g) {
    if (g.locked)
        printf_unlock(g.irql);
}

static void console_line(const char *s, uint8_t indent) {
    struct report_guard g = report_lock();

    printf_unlocked("%*s", (int) indent, "");
    printf_unlocked("%s", s);

    if (!term_plain())
        printf_unlocked("%s", ANSI_RESET);

    printf_unlocked("\n");
    report_unlock(g);
}

struct report_target report_console(void) {
    return (struct report_target){
        .panes = NULL, .width = term_size().cols, .indent = 0, .pane = 0};
}

struct report_target report_console_indent(uint16_t indent) {
    uint16_t cols = term_size().cols;

    if (indent > cols / 2)
        indent = (uint16_t) (cols / 2);

    return (struct report_target){.panes = NULL,
                                  .width = (uint16_t) (cols - indent),
                                  .indent = (uint8_t) indent,
                                  .pane = 0};
}

static void pane_push(struct report_panes *panes, uint32_t pane, const char *s,
                      bool rule) {
    if (pane >= panes->n)
        return;

    if (panes->nrows[pane] >= REPORT_PANE_ROWS) {
        panes->dropped[pane]++;
        return;
    }

    panes->rule[pane][panes->nrows[pane]] = rule;

    char *dst = panes->text[pane][panes->nrows[pane]++];
    size_t n = 0;

    /* The line writer bounded this by display columns, but store is bounded by
     * bytes, and escapes plus padding causes problems, so we advance a whole
     * sequence or a whole glyph at a time */
    const char *tail = term_plain() ? "" : ANSI_RESET;
    size_t cap =
        REPORT_PANE_LINE_MAX - strlen(tail) - strlen(glyph_ellipsis()) - 1;

    while (*s) {
        const char *start = s;

        if (*s == '\033') {
            s++;

            if (*s == '[') {
                s++;
                while (*s && (*s < '@' || *s > '~'))
                    s++;
            }

            if (*s)
                s++;
        } else {
            s++;
            while ((*s & 0xC0) == 0x80)
                s++;
        }

        size_t len = (size_t) (s - start);

        if (n + len > cap)
            break;

        memcpy(dst + n, start, len);
        n += len;
    }

    /* Not counting this as dropped, because the line is here, and
     * the marker says where it stops */
    if (*s)
        n += (size_t) snprintf(dst + n, (int) (REPORT_PANE_LINE_MAX - n),
                               "%s%s", tail, glyph_ellipsis());

    dst[n] = '\0';
}

struct report_target report_pane(struct report_panes *panes, uint32_t pane) {
    uint16_t w = (pane < panes->n) ? panes->width[pane] : term_size().cols;

    return (struct report_target){
        .panes = panes, .width = w, .indent = 0, .pane = (uint8_t) pane};
}

static void box_emit_line(struct report_box *b, const char *s);

static void target_emit(struct report_target *tgt, struct report_line *l,
                        bool rule) {
    if (tgt->box) {
        box_emit_line(tgt->box, report_line_str(l));
        report_line_reset(l);
        return;
    }

    if (!tgt->panes) {
        console_line(report_line_str(l), tgt->indent);
        report_line_reset(l);
        return;
    }

    if (tgt->indent) {
        /* Console path indents as it writes: a column stores the text, so
         * indent must be baked */
        char buf[REPORT_PANE_LINE_MAX];

        snprintf(buf, (int) sizeof(buf), "%*s%s", (int) tgt->indent, "",
                 report_line_str(l));
        pane_push(tgt->panes, tgt->pane, buf, rule);
    } else {
        pane_push(tgt->panes, tgt->pane, report_line_str(l), rule);
    }

    report_line_reset(l);
}

struct report_target report_target_indent(struct report_target tgt,
                                          uint16_t indent) {
    if (indent > tgt.width / 2)
        indent = (uint16_t) (tgt.width / 2);

    tgt.indent = (uint8_t) (tgt.indent + indent);
    tgt.width = (uint16_t) (tgt.width - indent);
    return tgt;
}

void report_line_emit(struct report_target *tgt, struct report_line *l) {
    target_emit(tgt, l, false);
}

void report_puts(struct report_target *tgt, const char *s) {
    REPORT_LINE(l, report_target_width(tgt));

    report_line_puts(&l, s);
    report_line_emit(tgt, &l);
}

void report_printf(struct report_target *tgt, const char *fmt, ...) {
    REPORT_LINE(l, report_target_width(tgt));
    va_list ap;

    va_start(ap, fmt);
    report_line_vprintf(&l, fmt, ap);
    va_end(ap);

    report_line_emit(tgt, &l);
}

void report_blank(struct report_target *tgt) {
    report_puts(tgt, "");
}

void report_rule_sev(struct report_target *tgt, enum term_sev sev,
                     const char *title) {
    REPORT_LINE(l, report_target_width(tgt));
    const char *hbar = glyph_hbar();

    report_line_puts(&l, term_style(TERM_SEV_DIM));
    report_line_repeat(&l, hbar, 2);

    if (title && *title) {
        report_line_puts(&l, " ");
        report_line_puts(&l, term_style(sev));
        report_line_puts(&l, title);
        report_line_puts(&l, term_style_reset());
        report_line_puts(&l, term_style(TERM_SEV_DIM));
        report_line_puts(&l, " ");
    }

    report_line_repeat(&l, hbar, l.max_col - l.col);

    /* Flagged so a column flush knows to grow a stub on the divider */
    target_emit(tgt, &l, true);
}

void report_rule(struct report_target *tgt, const char *title) {
    report_rule_sev(tgt, TERM_SEV_LABEL, title);
}

/* Styling active at a line break, replayed at the start of next line so
 * bold/colored runs survive being wrapped */
#define REPORT_WRAP_STYLE_MAX 64

#define REPORT_WRAP_MIN_COLS 8

struct report_wrap {
    struct report_target *tgt;
    struct report_line line;
    char style[REPORT_WRAP_STYLE_MAX];
    size_t style_len;
    char storage[REPORT_LINE_MAX];
};

static void wrap_note_style(struct report_wrap *w, const char *esc, size_t n) {
    bool reset =
        (n == 3 && esc[2] == 'm') || (n == 4 && esc[2] == '0' && esc[3] == 'm');

    if (reset) {
        w->style_len = 0;
        w->style[0] = '\0';
        return;
    }

    /* Drop if it can't fit */
    if (w->style_len + n >= sizeof(w->style))
        return;

    memcpy(w->style + w->style_len, esc, n);
    w->style_len += n;
    w->style[w->style_len] = '\0';
}

static void wrap_emit(struct report_wrap *w) {
    report_line_emit(w->tgt, &w->line);

    if (w->style_len)
        report_line_puts(&w->line, w->style);
}

/* End of the run of non-space starting at `s`, with its display width */
static const char *wrap_scan(const char *s, size_t *width) {
    size_t n = 0;

    while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
        if (*s == '\033') {
            s = esc_skip(s, s + strlen(s));
            continue;
        }

        s++;
        while ((*s & 0xC0) == 0x80)
            s++;

        n++;
    }

    *width = n;
    return s;
}

static size_t wrap_width(const char *s, const char *end) {
    size_t n = 0;

    while (s < end) {
        if (*s == '\033') {
            s = esc_skip(s, end);
            continue;
        }

        s++;
        while (s < end && (*s & 0xC0) == 0x80)
            s++;

        n++;
    }

    return n;
}

/* Advance at most `cols`, don't split glyphs */
static const char *wrap_take(const char *s, const char *end, size_t cols) {
    while (s < end && cols) {
        if (*s == '\033') {
            s = esc_skip(s, end);
            continue;
        }

        s++;
        while (s < end && (*s & 0xC0) == 0x80)
            s++;

        cols--;
    }

    return s;
}

static void wrap_copy(struct report_wrap *w, const char *s, const char *end) {
    for (const char *p = s; p < end;) {
        if (*p != '\033') {
            p++;
            continue;
        }

        const char *esc = p;

        p = esc_skip(p, end);
        wrap_note_style(w, esc, (size_t) (p - esc));
    }

    line_puts_n(&w->line, s, end);
}

void report_wrap(struct report_target *tgt, const char *text) {
    size_t width = report_target_width(tgt);
    struct report_wrap w;

    /* Too narrow to break */
    if (!text || !*text || width < REPORT_WRAP_MIN_COLS) {
        report_puts(tgt, text ? text : "");
        return;
    }

    w.tgt = tgt;
    w.style[0] = '\0';
    w.style_len = 0;
    report_line_init(&w.line, w.storage, sizeof(w.storage), width);

    while (*text) {
        /* Honor newlines */
        if (*text == '\n') {
            wrap_emit(&w);
            text++;
            continue;
        }

        if (*text == ' ' || *text == '\t') {
            text++;
            continue;
        }

        size_t word;
        const char *end = wrap_scan(text, &word);

        /* separating space counts against the budget as well */
        if (w.line.col && w.line.col + 1 + word > width)
            wrap_emit(&w);

        if (w.line.col)
            report_line_puts(&w.line, " ");

        if (w.line.col + word <= width) {
            wrap_copy(&w, text, end);
            text = end;
            continue;
        }

        /* Longer than a line on its own, breaking and marking breaks to show
         * when it's a break vs a sentence that just happened to wrap */
        while (text < end) {
            size_t room = width - w.line.col;

            if (wrap_width(text, end) <= room) {
                wrap_copy(&w, text, end);
                text = end;
                break;
            }

            /* after a flush, `room` is the full width */
            if (room < 4) {
                wrap_emit(&w);
                continue;
            }

            const char *stop = wrap_take(text, end, room - 1);

            wrap_copy(&w, text, stop);
            text = stop;

            report_line_puts(&w.line, term_style(TERM_SEV_DIM));
            report_line_puts(&w.line, glyph_continue());
            wrap_emit(&w);
        }
    }

    if (w.line.col)
        report_line_emit(tgt, &w.line);
}

void report_wrap_printf(struct report_target *tgt, const char *fmt, ...) {
    char buf[REPORT_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, (int) sizeof(buf), fmt, ap);
    va_end(ap);

    report_wrap(tgt, buf);
}

/* Boxes are borders + one space of padding, so it occupies inner + 4 columns
 * and the closing corner sits at inner + 3 */
#define REPORT_BOX_CHROME 4

void report_box_open(struct report_box *b, struct report_target target,
                     const char *title, uint16_t inner) {
    const char *h = glyph_hbar();
    uint16_t room = target.width > REPORT_BOX_CHROME
                        ? (uint16_t) (target.width - REPORT_BOX_CHROME)
                        : 1;

    if (!inner || inner > room)
        inner = room;

    b->target = target;
    b->inner = inner;

    REPORT_LINE(l, target.width);

    report_line_puts(&l, term_style(TERM_SEV_DIM));
    report_line_puts(&l, term_unicode() && !term_plain() ? "┌" : "+");
    report_line_puts(&l, h);

    if (title && *title) {
        report_line_puts(&l, " ");
        report_line_puts(&l, term_style(TERM_SEV_LABEL));
        report_line_puts(&l, title);
        report_line_puts(&l, term_style_reset());
        report_line_puts(&l, term_style(TERM_SEV_DIM));
        report_line_puts(&l, " ");
    }

    /* title longer than the box has eaten the top edge */
    if (l.col < (size_t) inner + 3)
        report_line_repeat(&l, h, (size_t) inner + 3 - l.col);

    report_line_puts(&l, term_unicode() && !term_plain() ? "┐" : "+");
    report_line_emit(&b->target, &l);
}

/* One already composed line, wrapped in the borders and passed on */
static void box_emit_line(struct report_box *b, const char *s) {
    const char *v = glyph_vbar();

    REPORT_LINE(l, b->target.width);

    report_line_puts(&l, term_style(TERM_SEV_DIM));
    report_line_puts(&l, v);
    report_line_puts(&l, term_style_reset());
    report_line_puts(&l, " ");

    report_line_field(&l, s, b->inner);

    report_line_puts(&l, " ");
    report_line_puts(&l, term_style(TERM_SEV_DIM));
    report_line_puts(&l, v);
    report_line_emit(&b->target, &l);
}

struct report_target report_box_body(struct report_box *b) {
    return (struct report_target){.box = b, .width = b->inner};
}

void report_box_printf(struct report_box *b, const char *fmt, ...) {
    struct report_target body = report_box_body(b);
    char buf[REPORT_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, (int) sizeof(buf), fmt, ap);
    va_end(ap);

    /* Wrapping, not clipping: the box grows downwards for free */
    report_wrap(&body, buf);
}

void report_box_close(struct report_box *b) {
    REPORT_LINE(l, b->target.width);

    report_line_puts(&l, term_style(TERM_SEV_DIM));
    report_line_puts(&l, term_unicode() && !term_plain() ? "└" : "+");
    report_line_repeat(&l, glyph_hbar(), (size_t) b->inner + 2);
    report_line_puts(&l, term_unicode() && !term_plain() ? "┘" : "+");
    report_line_emit(&b->target, &l);
}

static bool section_claim(struct report_target *tgt, const char *name) {
    if (report_skip_section && report_skip_section == name) {
        report_printf(tgt, "%s[%s skipped: faulted while rendering]%s",
                      term_style(TERM_SEV_WARN), name, term_style_reset());
        return false;
    }

    report_section = name;
    return true;
}

bool report_section_claim_at(struct report_target *tgt, const char *name) {
    return section_claim(tgt, name);
}

bool report_section_claim(const char *name) {
    struct report_target con = report_console();

    return section_claim(&con, name);
}

bool report_section_begin_at(struct report_target *tgt, const char *name) {
    if (!section_claim(tgt, name))
        return false;

    report_blank(tgt);
    report_rule_sev(tgt, TERM_SEV_LABEL, name);
    return true;
}

bool report_section_begin(const char *name) {
    struct report_target con = report_console();

    return report_section_begin_at(&con, name);
}

void report_section_end(void) {
    report_section = NULL;
}

const char *report_section_current(void) {
    return report_section;
}

static void fields_flush_row(struct report_fields *g) {
    if (!g->in_row)
        return;

    report_line_emit(&g->target, &g->line);
    g->in_row = 0;
}

void report_fields_begin(struct report_fields *g, struct report_target target,
                         uint32_t keyw, uint32_t valw) {
    uint32_t cell = keyw + 1 + valw + REPORT_FIELD_GAP;
    uint32_t per = cell ? (target.width + REPORT_FIELD_GAP) / cell : 1;

    g->target = target;
    g->keyw = (uint8_t) keyw;
    g->valw = (uint8_t) valw;
    g->per_row = per ? (uint8_t) per : 1;
    g->in_row = 0;

    report_line_init(&g->line, g->storage, sizeof(g->storage), target.width);
}

void report_field(struct report_fields *g, const char *key, const char *fmt,
                  ...) {
    char val[REPORT_PANE_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(val, (int) sizeof(val), fmt, ap);
    va_end(ap);

    if (g->in_row >= g->per_row)
        fields_flush_row(g);

    if (g->in_row)
        report_line_repeat(&g->line, " ", REPORT_FIELD_GAP);

    report_line_puts(&g->line, term_style(TERM_SEV_LABEL));
    report_line_field(&g->line, key, g->keyw);
    report_line_puts(&g->line, term_style_reset());
    report_line_puts(&g->line, " ");
    report_line_field(&g->line, val, g->valw);

    g->in_row++;
}

void report_field_full(struct report_fields *g, const char *key,
                       const char *fmt, ...) {
    char val[REPORT_LINE_MAX];
    va_list ap;

    fields_flush_row(g);

    va_start(ap, fmt);
    vsnprintf(val, (int) sizeof(val), fmt, ap);
    va_end(ap);

    report_line_puts(&g->line, term_style(TERM_SEV_LABEL));
    report_line_field(&g->line, key, g->keyw);
    report_line_puts(&g->line, term_style_reset());
    report_line_puts(&g->line, " ");
    report_line_puts(&g->line, val);
    report_line_emit(&g->target, &g->line);
}

void report_fields_end(struct report_fields *g) {
    fields_flush_row(g);
}

uint16_t report_pane_rows(const struct report_panes *panes, uint32_t pane) {
    return pane < panes->n ? panes->nrows[pane] : 0;
}

uint16_t report_panes_rows_max(const struct report_panes *panes) {
    uint16_t max = 0;

    for (uint32_t i = 0; i < panes->n; i++) {
        if (panes->nrows[i] > max)
            max = panes->nrows[i];
    }

    return max;
}

struct report_panes *report_panes_panic(void) {
    return &panic_panes;
}

void report_panes_begin(struct report_panes *panes, uint32_t n,
                        const uint8_t *weights) {
    uint16_t total = term_size().cols;
    uint32_t sum = 0;
    uint16_t used = 0;

    if (n < 1)
        n = 1;
    if (n > REPORT_PANES_MAX)
        n = REPORT_PANES_MAX;

    panes->n = (uint8_t) n;
    panes->stacked = false;
    panes->undivided = false;

    for (uint32_t i = 0; i < n; i++) {
        panes->nrows[i] = 0;
        panes->dropped[i] = 0;
        panes->title[i] = NULL;
        sum += weights ? weights[i] : 1;
    }

    if (!sum)
        sum = n;

    uint16_t gaps = (uint16_t) (REPORT_PANE_GAP * (n - 1));
    uint16_t avail = total > gaps ? (uint16_t) (total - gaps) : 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = weights ? weights[i] : 1;
        panes->width[i] = (uint16_t) ((avail * w) / sum);
        used = (uint16_t) (used + panes->width[i]);
    }

    /* Rounding to the last column */
    if (avail > used)
        panes->width[n - 1] = (uint16_t) (panes->width[n - 1] + (avail - used));

    for (uint32_t i = 0; i < n; i++) {
        if (panes->width[i] < REPORT_PANE_MIN_WIDTH)
            panes->stacked = true;
    }

    /* Stacked panels get full width each */
    if (panes->stacked) {
        for (uint32_t i = 0; i < n; i++)
            panes->width[i] = total;
    }
}

void report_panes_undivided(struct report_panes *panes) {
    panes->undivided = true;
}

void report_panes_title(struct report_panes *panes, uint32_t pane,
                        enum term_sev sev, const char *title) {
    if (pane >= panes->n)
        return;

    panes->title[pane] = title;
    panes->title_sev[pane] = (uint8_t) sev;
}

static void panes_flush_stacked(struct report_panes *panes) {
    struct report_target con = report_console();

    for (uint32_t i = 0; i < panes->n; i++) {
        if (panes->title[i])
            report_rule_sev(&con, TERM_SEV_LABEL, panes->title[i]);

        for (uint32_t r = 0; r < panes->nrows[i]; r++)
            report_puts(&con, panes->text[i][r]);

        if (panes->dropped[i])
            report_printf(&con, "%s    +%u more line(s) not shown%s",
                          term_style(TERM_SEV_WARN), panes->dropped[i],
                          term_style_reset());
    }
}

/* Display column the divider before col `i` occupies */
static size_t panes_divider_at(const struct report_panes *panes, uint32_t i) {
    size_t pos = 0;

    for (uint32_t k = 0; k < i; k++)
        pos += (size_t) panes->width[k] + REPORT_PANE_GAP;

    return pos - REPORT_PANE_GAP + 1;
}

static bool pane_row_is_rule(const struct report_panes *panes, uint32_t i,
                             uint16_t r) {
    return r < panes->nrows[i] && panes->rule[i][r];
}

/* The gap between two columns for a row, carrying
 * whichever rules exist here through the divider */
static void panes_divider(struct report_line *l,
                          const struct report_panes *panes, uint32_t i,
                          uint16_t r) {
    if (panes->undivided) {
        report_line_repeat(l, " ", REPORT_PANE_GAP);
        return;
    }

    bool left = pane_row_is_rule(panes, i - 1, r);
    bool right = pane_row_is_rule(panes, i, r);

    report_line_puts(l, term_style(TERM_SEV_DIM));
    report_line_puts(l, left ? glyph_hbar() : " ");
    report_line_puts(l, glyph_join(left, right));
    report_line_puts(l, right ? glyph_hbar() : " ");
    report_line_puts(l, term_style_reset());
}

static void panes_frame(struct report_panes *panes, enum term_sev sev,
                        const char *title, const char *tee) {
    struct report_target con = report_console();
    const char *h = glyph_hbar();

    /* Nothing to tie off if the columns were stacked or never divided */
    if (!panes->n || panes->stacked || panes->undivided) {
        report_rule_sev(&con, sev, title);
        return;
    }

    REPORT_LINE(l, report_target_width(&con));

    report_line_puts(&l, term_style(TERM_SEV_DIM));
    report_line_repeat(&l, h, 2);

    if (title && *title) {
        report_line_puts(&l, " ");
        report_line_puts(&l, term_style(sev));
        report_line_puts(&l, title);
        report_line_puts(&l, term_style_reset());
        report_line_puts(&l, term_style(TERM_SEV_DIM));
        report_line_puts(&l, " ");
    }

    for (uint32_t i = 1; i < panes->n; i++) {
        size_t at = panes_divider_at(panes, i);

        /* title long enough to reach the divider has passed it */
        if (l.col < at)
            report_line_repeat(&l, h, at - l.col);

        report_line_puts(&l, tee);
    }

    report_line_repeat(&l, h, l.max_col - l.col);
    report_line_emit(&con, &l);
}

/* A titled segment per column, a column's first heading
 * costs no rows, since it's the top edge */

/* Divider columns of a region that has finished, waiting for the next opening
 * rule to tie them off. Kept out here because report_panes_begin() wipes the
 * struct the widths were derived from */
static struct {
    uint16_t at[REPORT_PANES_MAX];
    uint32_t n;
} panes_pending;

void report_panes_carry(struct report_panes *panes) {
    panes_pending.n = 0;

    if (panes->stacked || panes->undivided)
        return;

    for (uint32_t i = 1; i < panes->n; i++)
        panes_pending.at[panes_pending.n++] =
            (uint16_t) panes_divider_at(panes, i);
}

static void panes_title_segment(struct report_line *l,
                                const struct report_panes *panes, uint32_t i) {
    report_line_puts(l, term_style(TERM_SEV_DIM));
    report_line_repeat(l, glyph_hbar(), 2);

    if (!panes->title[i] || !*panes->title[i])
        return;

    report_line_puts(l, " ");
    report_line_puts(l, term_style((enum term_sev) panes->title_sev[i]));
    report_line_puts(l, panes->title[i]);
    report_line_puts(l, term_style_reset());
    report_line_puts(l, term_style(TERM_SEV_DIM));
    report_line_puts(l, " ");
}

void report_panes_top(struct report_panes *panes) {
    struct report_target con = report_console();
    const char *h = glyph_hbar();
    uint16_t open_at[REPORT_PANES_MAX];
    uint32_t n_open = 0;
    uint32_t oi = 0, ci = 0;

    if (!panes->n) {
        panes_pending.n = 0;
        return;
    }

    if (panes->stacked) {
        /* No divider to hang the later segments off */
        report_rule_sev(&con, panes->title_sev[0], panes->title[0]);
        panes_pending.n = 0;
        return;
    }

    if (!panes->undivided) {
        for (uint32_t i = 1; i < panes->n; i++)
            open_at[n_open++] = (uint16_t) panes_divider_at(panes, i);
    }

    REPORT_LINE(l, report_target_width(&con));

    panes_title_segment(&l, panes, 0);

    /* Two ascending lists of divider columns merged so a position
     * in both gets a crossing instead of two rules a line apart */
    while (oi < n_open || ci < panes_pending.n) {
        size_t at;

        if (oi >= n_open)
            at = panes_pending.at[ci];
        else if (ci >= panes_pending.n)
            at = open_at[oi];
        else
            at = open_at[oi] < panes_pending.at[ci] ? open_at[oi]
                                                    : panes_pending.at[ci];

        bool below = oi < n_open && open_at[oi] == at;
        bool above = ci < panes_pending.n && panes_pending.at[ci] == at;
        uint32_t pane = oi + 1;

        if (below)
            oi++;
        if (above)
            ci++;

        /* A title long enough to reach the divider has passed it */
        if (l.col < at)
            report_line_repeat(&l, h, at - l.col);

        report_line_puts(&l, term_style(TERM_SEV_DIM));
        report_line_puts(&l, glyph_tee(above, below));

        if (below)
            panes_title_segment(&l, panes, pane);
    }

    panes_pending.n = 0;

    report_line_repeat(&l, h, l.max_col - l.col);
    report_line_emit(&con, &l);
}

void report_panes_bottom(struct report_panes *panes, enum term_sev sev,
                         const char *title) {
    panes_pending.n = 0;
    panes_frame(panes, sev, title, glyph_tee_up());
}

void report_panes_flush(struct report_panes *panes) {
    struct report_target con = report_console();
    uint16_t maxlines = 0;
    bool any_dropped = false;

    if (!panes->n)
        return;

    if (panes->stacked) {
        panes_flush_stacked(panes);
        return;
    }

    for (uint32_t i = 0; i < panes->n; i++) {
        if (panes->nrows[i] > maxlines)
            maxlines = panes->nrows[i];
        if (panes->dropped[i])
            any_dropped = true;
    }

    for (uint16_t r = 0; r < maxlines; r++) {
        REPORT_LINE(l, report_target_width(&con));

        for (uint32_t i = 0; i < panes->n; i++) {
            if (i)
                panes_divider(&l, panes, i, r);

            report_line_field(&l, r < panes->nrows[i] ? panes->text[i][r] : "",
                              panes->width[i]);
        }

        report_line_emit(&con, &l);
    }

    if (!any_dropped)
        return;

    for (uint32_t i = 0; i < panes->n; i++) {
        if (!panes->dropped[i])
            continue;

        report_printf(&con, "%s[%s: +%u more line(s) not shown]%s",
                      term_style(TERM_SEV_WARN),
                      panes->title[i] ? panes->title[i] : "panel",
                      panes->dropped[i], term_style_reset());
    }
}

void report_enter_panic(void) {
    bool nested = term_in_panic();
    const char *sec = report_section;

    report_section = NULL;
    term_enter_panic();

    if (!nested)
        return;

    /* Re-entered with a section open: that section is what killed us */
    struct report_target con = report_console();

    report_blank(&con);

    if (sec) {
        report_skip_section = sec;
        report_printf(&con,
                      "%s!! nested panic while rendering [%s] - skipping it%s",
                      term_style(TERM_SEV_CRIT), sec, term_style_reset());
    } else {
        report_printf(&con, "%s!! nested panic outside any section%s",
                      term_style(TERM_SEV_CRIT), term_style_reset());
    }
}
