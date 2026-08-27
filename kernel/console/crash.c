/* @title: Kernel Crash Engine */
#include <acpi/lapic.h>
#include <asm.h>
#include <bootstage.h>
#include <console/crash.h>
#include <console/panic_scene.h>
#include <console/printf.h>
#include <console/report.h>
#include <console/statusbar.h>
#include <console/term.h>
#include <dbg.h>
#include <global.h>
#include <irq/irq.h>
#include <linker/symbols.h>
#include <log.h>
#include <logo.h>
#include <ndjson.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <stdarg.h>
#include <string.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <time/spin_sleep.h>
#include <time/time.h>

static _Atomic int64_t crash_owner = -1;
static _Atomic uint32_t crash_depth = 0;
static struct spinlock crash_lock = SPINLOCK_INIT;

PERCPU_DECLARE(crash_quiesced, _Atomic uint32_t, NULL);
PERCPU_DECLARE(crash_regs, struct panic_regs, NULL);
static struct panic_regs boot_crash_regs = {0};

extern void crash_capture_regs(struct panic_regs *out);

bool crash_cpu_is_owner(uint64_t id) {
    return atomic_load_explicit(&crash_owner, memory_order_relaxed) ==
           (int64_t) id;
}

void crash_nmi_handoff(void *p, struct irq_context *irqc) {
    (void) p;
    struct panic_regs *this_regs_buf = PERCPU_PTR(crash_regs);
    this_regs_buf->r8 = irqc->r8;
    this_regs_buf->r9 = irqc->r9;
    this_regs_buf->r10 = irqc->r10;
    this_regs_buf->r11 = irqc->r11;
    this_regs_buf->r12 = irqc->r12;
    this_regs_buf->r13 = irqc->r13;
    this_regs_buf->r14 = irqc->r14;
    this_regs_buf->r15 = irqc->r15;
    this_regs_buf->rdi = irqc->rdi;
    this_regs_buf->rsi = irqc->rsi;
    this_regs_buf->rbp = irqc->rbp;
    this_regs_buf->rsp = irqc->rsp;
    this_regs_buf->rax = irqc->rax;
    this_regs_buf->rbx = irqc->rbx;
    this_regs_buf->rcx = irqc->rcx;
    this_regs_buf->rdx = irqc->rdx;
    this_regs_buf->rip = irqc->rip;
    this_regs_buf->rflags = irqc->rflags;
    this_regs_buf->cr2 = read_cr2();
    this_regs_buf->cr3 = read_cr3();
    atomic_store(PERCPU_PTR(crash_quiesced), 1);
    while (1)
        hcf();
}

void crash_broadcast_nmi(void) {
    panic_broadcast(smp_core_id());
}

void panic_handler(struct panic_regs *regs) {
    disable_interrupts();

    if (PERCPU_READY(crash_regs)) {
        PERCPU_READ(crash_regs) = *regs;
    } else {
        boot_crash_regs = *regs;
    }

    if (global.current_bootstage >= BOOTSTAGE_MID_MP) {
        crash_broadcast_nmi();
        sleep_spin_ms(50);
    }
}

static bool crash_regs_captured(const struct panic_regs *r) {
    for (int i = 0; i < CRASH_REG_COUNT; i++) {
        if (r->regs[i])
            return true;
    }
    return false;
}

struct crash_reg_entry {
    const char *name;
    uint64_t val;
    bool ret_addr;
};

static size_t crash_regs_collect(const struct panic_regs *r,
                                 struct crash_reg_entry *out) {
    size_t n = 0;
    out[n++] = (struct crash_reg_entry){"rax", r->rax, false};
    out[n++] = (struct crash_reg_entry){"rbx", r->rbx, false};
    out[n++] = (struct crash_reg_entry){"rcx", r->rcx, false};
    out[n++] = (struct crash_reg_entry){"rdx", r->rdx, false};
    out[n++] = (struct crash_reg_entry){"rsi", r->rsi, false};
    out[n++] = (struct crash_reg_entry){"rdi", r->rdi, false};
    out[n++] = (struct crash_reg_entry){"rbp", r->rbp, false};
    out[n++] = (struct crash_reg_entry){"rsp", r->rsp, false};
    out[n++] = (struct crash_reg_entry){" r8", r->r8, false};
    out[n++] = (struct crash_reg_entry){" r9", r->r9, false};
    out[n++] = (struct crash_reg_entry){"r10", r->r10, false};
    out[n++] = (struct crash_reg_entry){"r11", r->r11, false};
    out[n++] = (struct crash_reg_entry){"r12", r->r12, false};
    out[n++] = (struct crash_reg_entry){"r13", r->r13, false};
    out[n++] = (struct crash_reg_entry){"r14", r->r14, false};
    out[n++] = (struct crash_reg_entry){"r15", r->r15, false};
    return n;
}

static void crash_describe_addr(uint64_t v, char *out, size_t cap) {
    uint64_t off = 0;
    const char *sym;

    if (v >= (uint64_t) &__stext && v < (uint64_t) &__etext) {
        sym = debug_symbolize(v, &off);
        if (sym)
            snprintf(out, (int) cap, "%s+0x%lx", sym, off);
        else
            snprintf(out, (int) cap, "<text>");
        return;
    }

    if (v >= (uint64_t) &__srodata && v < (uint64_t) &__erodata)
        snprintf(out, (int) cap, "<rodata>");
    else if (v >= (uint64_t) &__sdata && v < (uint64_t) &__edata)
        snprintf(out, (int) cap, "<data>");
    else if (v >= (uint64_t) &__sbss && v < (uint64_t) &__ebss)
        snprintf(out, (int) cap, "<bss>");
    else
        snprintf(out, (int) cap, "<none>");
}

static void crash_reg_str(char *out, size_t cap, const char *name, uint64_t v,
                          const char *note) {
    snprintf(out, (int) cap, "%s%s%s %016lx  %s%s%s",
             term_style(TERM_SEV_LABEL), name, term_style_reset(), v,
             term_style(TERM_SEV_DIM), note, term_style_reset());
}

static void crash_reg_fmt(char *out, size_t cap, const char *name, uint64_t v,
                          bool ret_addr) {
    char note[REPORT_PANE_LINE_MAX];
    crash_describe_addr(ret_addr && v ? v - 1 : v, note, sizeof(note));
    crash_reg_str(out, cap, name, v, note);
}

static void crash_reg_note(struct report_target *tgt, const char *name,
                           uint64_t v, const char *note) {
    char line[REPORT_PANE_LINE_MAX];
    crash_reg_str(line, sizeof(line), name, v, note);
    report_puts(tgt, line);
}

static void crash_reg_line(struct report_target *tgt, const char *name,
                           uint64_t v, bool ret_addr) {
    char line[REPORT_PANE_LINE_MAX];
    crash_reg_fmt(line, sizeof(line), name, v, ret_addr);
    report_puts(tgt, line);
}

#define CRASH_REG_FIELD_MIN 28
#define CRASH_REG_FIELD_GAP 2

static void crash_reg_field(struct report_line *l,
                            const struct crash_reg_entry *r, size_t width) {
    char cell[REPORT_PANE_LINE_MAX];
    crash_reg_fmt(cell, sizeof(cell), r->name, r->val, r->ret_addr);
    report_line_field(l, cell, width);
}

static void crash_regs_grid(struct report_target *tgt,
                            const struct crash_reg_entry *r, size_t n) {
    size_t width = report_target_width(tgt);
    size_t per_row = 1;
    size_t cell = width;

    if (width >= 2 * CRASH_REG_FIELD_MIN + CRASH_REG_FIELD_GAP) {
        per_row = 2;
        cell = (width - CRASH_REG_FIELD_GAP) / 2;
    }

    for (size_t i = 0; i < n; i += per_row) {
        REPORT_LINE(l, width);
        crash_reg_field(&l, &r[i], cell);
        if (per_row == 2 && i + 1 < n) {
            report_line_repeat(&l, " ", CRASH_REG_FIELD_GAP);
            crash_reg_field(&l, &r[i + 1], cell);
        }
        report_line_emit(tgt, &l);
    }
}

static void crash_rflags_decode(uint64_t f, char *out, size_t cap) {
    static const struct {
        uint8_t bit;
        const char *name;
    } bits[] = {{0, "CF"},  {2, "PF"},  {4, "AF"},  {6, "ZF"},
                {7, "SF"},  {8, "TF"},  {9, "IF"},  {10, "DF"},
                {11, "OF"}, {16, "RF"}, {17, "VM"}, {18, "AC"}};

    size_t n = 0;
    if (cap < 2)
        return;

    out[n++] = '[';
    out[n] = '\0';

    for (size_t i = 0; i < sizeof(bits) / sizeof(*bits); i++) {
        if (!(f & (1ull << bits[i].bit)))
            continue;
        if (n + 5 >= cap)
            break;
        n += (size_t) snprintf(out + n, (int) (cap - n), "%s%s",
                               n > 1 ? " " : "", bits[i].name);
    }

    if (n + 1 < cap)
        snprintf(out + n, (int) (cap - n), "] iopl=%lu", (f >> 12) & 3);
}

static void crash_regs_panel(struct report_target *tgt,
                             const struct panic_regs *regs) {
    struct crash_reg_entry r[CRASH_REG_COUNT];
    size_t n;

    if (!crash_regs_captured(regs)) {
        report_printf(tgt, "%s<no registers>%s", term_style(TERM_SEV_WARN),
                      term_style_reset());
        return;
    }

    char flags[REPORT_PANE_LINE_MAX];
    crash_reg_line(tgt, "rip", regs->rip, true);
    crash_rflags_decode(regs->rflags, flags, sizeof(flags));
    crash_reg_note(tgt, "flg", regs->rflags, flags);
    crash_reg_line(tgt, "cr2", regs->cr2, false);
    crash_reg_note(tgt, "cr3", regs->cr3, "<phys>");

    report_puts(tgt, "");
    n = crash_regs_collect(regs, r);
    crash_regs_grid(tgt, r, n);
}

static void crash_backtrace_panel(struct report_target *tgt,
                                  const struct panic_regs *regs) {
    uint64_t entries[STACK_TRACE_MAX_DEPTH];
    size_t nr = 0;

    if (crash_regs_captured(regs) && regs->rip)
        entries[nr++] = regs->rip;

    if (global.current_bootstage >= BOOTSTAGE_EARLY_ALLOCATORS) {
        if (crash_regs_captured(regs) && regs->rbp)
            nr += stack_unwind(regs->rbp, entries + nr,
                               STACK_TRACE_MAX_DEPTH - nr);
        else
            nr += stack_unwind((uint64_t) __builtin_frame_address(0),
                               entries + nr, STACK_TRACE_MAX_DEPTH - nr);
    }

    if (!debug_syms_present())
        report_printf(tgt, "%s<no symbol table: rebuild to symbolize>%s",
                      term_style(TERM_SEV_WARN), term_style_reset());

    if (!nr) {
        report_printf(tgt, "%s<no kernel frames found>%s",
                      term_style(TERM_SEV_WARN), term_style_reset());
        return;
    }

    for (size_t i = 0; i < nr; i++) {
        uint64_t off = 0;
        const char *sym = debug_symbolize(entries[i], &off);
        uint32_t line = 0;
        const char *file;
        char frame[REPORT_PANE_LINE_MAX];
        char at[REPORT_PANE_LINE_MAX];

        if (sym)
            snprintf(frame, (int) sizeof(frame),
                     "%s#%-2zu%s %012lx %s%s+0x%lx%s", term_style(TERM_SEV_DIM),
                     i, term_style_reset(), entries[i],
                     term_style(TERM_SEV_HEAD), sym, off, term_style_reset());
        else
            snprintf(frame, (int) sizeof(frame), "%s#%-2zu%s %012lx <unknown>",
                     term_style(TERM_SEV_DIM), i, term_style_reset(),
                     entries[i]);

        file = debug_line_for(entries[i] - 1, &line);
        if (!file) {
            report_puts(tgt, frame);
            continue;
        }

        snprintf(at, (int) sizeof(at), "%sat %s:%u%s", term_style(TERM_SEV_DIM),
                 file, line, term_style_reset());

        if (report_strwidth(frame) + 2 + report_strwidth(at) <=
            report_target_width(tgt)) {
            report_printf(tgt, "%s  %s", frame, at);
        } else {
            report_puts(tgt, frame);
            report_printf(tgt, "    %s", at);
        }
    }
}

#define CRASH_CPU_MIN_WIDTH 52

static uint32_t crash_cpu_panes(void) {
    uint16_t total = term_size().cols;
    for (uint32_t n = REPORT_PANES_MAX; n > 1; n--) {
        if (total >= n * CRASH_CPU_MIN_WIDTH + (n - 1) * REPORT_PANE_GAP)
            return n;
    }
    return 1;
}

static void crash_cpu_box(struct report_target *tgt, uint64_t id,
                          uint16_t inner) {
    const struct panic_regs *r = PERCPU_PTR_FOR_CPU(crash_regs, id);
    _Atomic uint32_t *quiesced = PERCPU_PTR_FOR_CPU(crash_quiesced, id);
    time_us_t end = time_get_us() + CRASH_WAIT_US;
    uint64_t entries[6];
    char line[REPORT_PANE_LINE_MAX];
    char title[24];
    struct report_box box;
    size_t nr;

    snprintf(title, (int) sizeof(title), "cpu %lu", id);

    while (!atomic_load(quiesced) && time_get_us() <= end)
        sleep_spin_us(CRASH_SPIN_ONE_US);

    report_box_open(&box, *tgt, title, inner);

    if (!atomic_load(quiesced)) {
        report_box_printf(&box, "%sno response to the crash NMI%s",
                          term_style(TERM_SEV_WARN), term_style_reset());
        report_box_close(&box);
        return;
    }

    crash_reg_fmt(line, sizeof(line), "rip", r->rip, true);
    report_box_printf(&box, "%s", line);
    crash_reg_fmt(line, sizeof(line), "rsp", r->rsp, false);
    report_box_printf(&box, "%s", line);
    crash_reg_fmt(line, sizeof(line), "rbp", r->rbp, false);
    report_box_printf(&box, "%s", line);

    nr = r->rbp ? stack_unwind(r->rbp, entries, 6) : 0;
    if (!nr) {
        report_box_printf(&box, "%s<no frames>%s", term_style(TERM_SEV_DIM),
                          term_style_reset());
        report_box_close(&box);
        return;
    }

    for (size_t i = 0; i < nr; i++) {
        uint64_t off = 0;
        const char *sym = debug_symbolize(entries[i], &off);
        if (sym)
            report_box_printf(&box, "%s#%-2zu%s %s+0x%lx",
                              term_style(TERM_SEV_DIM), i, term_style_reset(),
                              sym, off);
        else
            report_box_printf(&box, "%s#%-2zu%s %016lx",
                              term_style(TERM_SEV_DIM), i, term_style_reset(),
                              entries[i]);
    }

    report_box_close(&box);
}

static void crash_other_cpus(struct report_panes *panes) {
    struct report_target col0 = report_pane(panes, 0);
    uint64_t self = smp_core_id();
    uint16_t inner = 0;
    uint32_t count = 0;
    uint32_t per_col;
    uint32_t slot = 0;
    uint64_t id;

    if (global.current_bootstage < BOOTSTAGE_MID_MP) {
        report_printf(&col0, "  %s<single core at this bootstage>%s",
                      term_style(TERM_SEV_DIM), term_style_reset());
        return;
    }

    if (!PERCPU_READY(crash_regs)) {
        report_printf(&col0, "  %s<percpu regs not initialised yet>%s",
                      term_style(TERM_SEV_DIM), term_style_reset());
        return;
    }

    for_each_cpu_id(id) {
        if (id != self)
            count++;
    }

    if (!count) {
        report_printf(&col0, "  %s<no other cores>%s", term_style(TERM_SEV_DIM),
                      term_style_reset());
        return;
    }

    for (uint32_t i = 0; i < panes->n; i++) {
        struct report_target s = report_pane(panes, i);
        uint16_t room = s.width > 4 ? (uint16_t) (s.width - 4) : 1;
        if (!inner || room < inner)
            inner = room;
    }

    per_col = (count + panes->n - 1) / panes->n;
    if (!per_col)
        per_col = 1;

    for_each_cpu_id(id) {
        if (id == self)
            continue;

        uint32_t which = slot++ / per_col;
        struct report_target target = report_pane(
            panes, which < panes->n ? which : (uint32_t) panes->n - 1);
        crash_cpu_box(&target, id, inner);
    }
}

#define CRASH_LOGO_INDENT 2

static size_t crash_logo_indent(const char *logo) {
    size_t least = (size_t) -1;
    for (const char *p = logo; *p;) {
        size_t indent = 0;
        while (*p == ' ') {
            indent++;
            p++;
        }
        if (*p && *p != '\n' && indent < least)
            least = indent;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return least == (size_t) -1 ? 0 : least;
}

static size_t crash_logo_width(const char *logo, size_t skip) {
    size_t widest = 0;
    for (const char *p = logo; *p;) {
        size_t n = 0;
        for (size_t i = 0; i < skip && *p == ' '; i++)
            p++;
        while (*p && *p != '\n') {
            n++;
            p++;
        }
        if (n > widest)
            widest = n;
        if (*p)
            p++;
    }
    return widest;
}

static void crash_logo_panel(struct report_target *tgt, const char *logo) {
    size_t skip = crash_logo_indent(logo);
    size_t art = crash_logo_width(logo, skip);
    size_t width = report_target_width(tgt);
    size_t pad = art < width ? (width - art) / 2 : 0;

    for (const char *p = logo; *p;) {
        char row[REPORT_PANE_LINE_MAX];
        size_t n = 0;
        for (size_t i = 0; i < skip && *p == ' '; i++)
            p++;
        while (*p && *p != '\n' && n + 1 < sizeof(row))
            row[n++] = *p++;
        while (n && row[n - 1] == ' ')
            n--;
        row[n] = '\0';
        if (*p)
            p++;
        report_printf(tgt, "%*s%s%s%s", (int) pad, "", ANSI_RED, row,
                      ANSI_RESET);
    }
}

static void crash_where_box(struct report_target *tgt, const char *file,
                            int line, const char *func) {
    const char *sep = term_unicode() ? " · " : " | ";
    char loc[REPORT_LINE_MAX];
    char ctx[REPORT_LINE_MAX];
    struct report_box box;
    size_t w;

    snprintf(loc, (int) sizeof(loc), "%s%s:%d%s %s%s()%s", ANSI_GREEN,
             file ? file : "<unknown>", line, ANSI_RESET, ANSI_CYAN,
             func ? func : "<unknown>", ANSI_RESET);

    char *thread_name = global.current_bootstage >= BOOTSTAGE_LATE
                            ? thread_get_current()->name
                            : "(null)";
    if (global.current_bootstage < BOOTSTAGE_EARLY_DEVICES)
        snprintf(ctx, (int) sizeof(ctx),
                 "cpu %lu%stime unknown%sbootstage '%s'", smp_core_id(), sep,
                 sep, bootstage_str[global.current_bootstage]);
    else
        snprintf(ctx, (int) sizeof(ctx),
                 "cpu %lu%s%lu ms%sbootstage '%s'\nthread '%s'", smp_core_id(),
                 sep, time_get_ms(), sep,
                 bootstage_str[global.current_bootstage], thread_name);

    w = report_strwidth(loc);
    if (report_strwidth(ctx) > w)
        w = report_strwidth(ctx);

    report_box_open(&box, *tgt, "where", (uint16_t) w);
    report_box_printf(&box, "%s", loc);
    report_box_printf(&box, "%s%s%s", term_style(TERM_SEV_DIM), ctx,
                      term_style_reset());
    report_box_close(&box);
}

static void crash_empty_box_panel(struct report_target *tgt) {
    for (size_t i = 0; i < panic_scene_count; i++)
        report_puts(tgt, panic_scene[i]);
}

NDJSON_DECLARE(panic_at, NDJSON_DOMAIN_PANIC, NDJSON_KIND_AT, 1,
               NDJSON_STR(file), NDJSON_U64(line), NDJSON_STR(func),
               NDJSON_STR(msg), NDJSON_STR(bootstage), NDJSON_STR(thread),
               NDJSON_U64(depth));

NDJSON_DECLARE(panic_frame, NDJSON_DOMAIN_PANIC, NDJSON_KIND_FRAME, 1,
               NDJSON_U64(idx), NDJSON_HEX(addr), NDJSON_STR(sym),
               NDJSON_U64(off), NDJSON_STR(file), NDJSON_U64(line));

static void crash_emit_ndjson_records(const struct crash_context *ctx,
                                      const struct panic_regs *regs,
                                      uint32_t depth) {
    const char *thread = global.current_bootstage >= BOOTSTAGE_LATE
                             ? thread_get_current()->name
                             : NULL;

    ndjson_emit(panic_at, .file = ctx->file ? ctx->file : "",
                .line = (uint64_t) ctx->line,
                .func = ctx->func ? ctx->func : "",
                .msg = ctx->msg ? ctx->msg : "",
                .bootstage = bootstage_str[global.current_bootstage],
                .thread = thread, .depth = depth);

    uint64_t entries[STACK_TRACE_MAX_DEPTH];
    size_t nr = 0;

    if (regs && regs->rip)
        entries[nr++] = regs->rip;

    if (global.current_bootstage >= BOOTSTAGE_EARLY_ALLOCATORS) {
        if (regs && regs->rbp)
            nr += stack_unwind(regs->rbp, entries + nr,
                               STACK_TRACE_MAX_DEPTH - nr);
        else
            nr += stack_unwind((uint64_t) __builtin_frame_address(0),
                               entries + nr, STACK_TRACE_MAX_DEPTH - nr);
    }

    for (size_t i = 0; i < nr; i++) {
        uint64_t off = 0;
        uint32_t srcline = 0;
        const char *sym = debug_symbolize(entries[i], &off);
        const char *srcfile = debug_line_for(entries[i] - 1, &srcline);

        ndjson_emit(panic_frame, .idx = i, .addr = entries[i], .sym = sym,
                    .off = off, .file = srcfile, .line = srcline);
    }
}

static const char *crash_source_title(enum crash_source s) {
    switch (s) {
    case CRASH_SOURCE_PANIC: return "kernel panic";
    case CRASH_SOURCE_ASSERT: return "assertion failure";
    case CRASH_SOURCE_KASAN: return "kasan fault";
    case CRASH_SOURCE_UBSAN: return "ubsan violation";
    case CRASH_SOURCE_NMI_WATCHDOG: return "watchdog lockup";
    case CRASH_SOURCE_CPU_EXCEPTION: return "cpu exception";
    case CRASH_SOURCE_NIGHTMARE: return "nightmare finding";
    default: return "kernel crash";
    }
}

static const char *crash_source_name(enum crash_source s) {
    switch (s) {
    case CRASH_SOURCE_PANIC: return "PANIC";
    case CRASH_SOURCE_ASSERT: return "ASSERTION FAILED";
    case CRASH_SOURCE_KASAN: return "KASAN";
    case CRASH_SOURCE_UBSAN: return "UBSAN";
    case CRASH_SOURCE_NMI_WATCHDOG: return "WATCHDOG";
    case CRASH_SOURCE_CPU_EXCEPTION: return "CPU EXCEPTION";
    case CRASH_SOURCE_NIGHTMARE: return "NIGHTMARE";
    default: return "CRASH";
    }
}

static void crash_report_visual(const struct crash_context *ctx,
                                const struct panic_regs *regs) {
    struct report_target con = report_console();
    struct report_panes *panes = report_panes_panic();

    printf("\033[H\033[2J");

    report_panes_begin(panes, 2, (const uint8_t[]){42, 58});
    const char *title = crash_source_title(ctx->source);
    report_panes_title(panes, 0, TERM_SEV_CRIT, title);
    report_panes_title(panes, 1, TERM_SEV_LABEL, "registers");
    report_panes_top(panes);

    struct report_target left = report_pane(panes, 0);
    struct report_target right = report_pane(panes, 1);

    crash_logo_panel(&left, OS_LOGO_PANIC_CENTERED);
    report_blank(&left);

    crash_where_box(&left, ctx->file, ctx->line, ctx->func);

    if (report_section_begin_at(&left, "message")) {
        report_wrap_printf(&left, "%s%s%s", term_style(TERM_SEV_HEAD),
                           ctx->msg ? ctx->msg : "<no message>",
                           term_style_reset());
    }
    report_section_end();

    if (report_section_claim_at(&right, "registers"))
        crash_regs_panel(&right, regs);
    report_section_end();

    if (report_section_begin_at(&right, "backtrace"))
        crash_backtrace_panel(&right, regs);
    report_section_end();

    if (report_section_begin_at(&right, "empty box"))
        crash_empty_box_panel(&right);
    report_section_end();

    report_panes_flush(panes);

    if (ctx->formats & CRASH_FMT_PEER_CPUS) {
        report_panes_carry(panes);
        report_panes_begin(panes, crash_cpu_panes(), NULL);
        report_panes_undivided(panes);
        report_panes_title(panes, 0, TERM_SEV_LABEL, "other CPUs");
        report_panes_top(panes);

        struct report_target cpu0 = report_pane(panes, 0);
        if (report_section_claim_at(&cpu0, "other CPUs"))
            crash_other_cpus(panes);
        report_section_end();
        report_panes_flush(panes);
    }

    if (ctx->formats & CRASH_FMT_DUMP_LOGS) {
        report_panes_bottom(panes, TERM_SEV_LABEL, "logs");
        if (report_section_claim("logs"))
            log_dump_panic();
        report_section_end();
    }

    report_blank(&con);
}

static void crash_report_raw_serial(const struct crash_context *ctx,
                                    const struct panic_regs *regs) {
    printf_unlocked("\n*** KERNEL CRASH: %s ***\n",
                    crash_source_name(ctx->source));
    if (ctx->file || ctx->func)
        printf_unlocked("Location: %s:%d in %s()\n",
                        ctx->file ? ctx->file : "<unknown>", ctx->line,
                        ctx->func ? ctx->func : "<unknown>");
    if (ctx->msg)
        printf_unlocked("Message : %s\n", ctx->msg);
    if (regs) {
        printf_unlocked(
            "RIP: %016lx  RSP: %016lx  RFLAGS: %016lx  CR2: %016lx\n",
            regs->rip, regs->rsp, regs->rflags, regs->cr2);
        printf_unlocked("RAX: %016lx  RBX: %016lx  RCX: %016lx  RDX: %016lx\n",
                        regs->rax, regs->rbx, regs->rcx, regs->rdx);
    }
}

__noreturn void crash(const struct crash_context *ctx) {
    disable_interrupts();

    uint32_t depth =
        atomic_fetch_add_explicit(&crash_depth, 1, memory_order_relaxed);

    if (depth >= CRASH_MAX_DEPTH) {
        printf_unlocked("\n[crash depth %u, aborting report]\n", depth);
#if defined(TEST_ENABLED) || defined(TEST_NIGHTMARE_ENABLED)
        ndjson_bye(QEMU_EXIT_PANIC, "nested_crash");
        qemu_exit(QEMU_EXIT_PANIC);
#endif
        while (true)
            wait_for_interrupt();
    }

    if (depth == 0)
        spin_lock_raw(&crash_lock);

    int64_t unowned = -1;
    atomic_compare_exchange_strong(&crash_owner, &unowned,
                                   (int64_t) smp_core_id());
    atomic_store(&global.panicked, true);

    struct panic_regs captured_regs;
    if (ctx->regs) {
        captured_regs = *ctx->regs;
    } else {
        crash_capture_regs(&captured_regs);
    }

    if (PERCPU_READY(crash_regs)) {
        PERCPU_READ(crash_regs) = captured_regs;
    } else {
        boot_crash_regs = captured_regs;
    }

    if (global.current_bootstage >= BOOTSTAGE_MID_MP) {
        crash_broadcast_nmi();
        sleep_spin_ms(50);
    }

    report_enter_panic();
    ndjson_enter_panic();

    uint32_t formats = ctx->formats;
    if (global.current_bootstage < BOOTSTAGE_EARLY_DEVICES) {
        formats &= ~CRASH_FMT_VISUAL_PANES;
        formats |= CRASH_FMT_RAW_SERIAL;
    }

    if (formats & CRASH_FMT_NDJSON)
        crash_emit_ndjson_records(ctx, &captured_regs, depth);

    if (formats & CRASH_FMT_VISUAL_PANES)
        crash_report_visual(ctx, &captured_regs);
    else if (formats & CRASH_FMT_RAW_SERIAL)
        crash_report_raw_serial(ctx, &captured_regs);

    if ((formats & CRASH_FMT_DUMP_LOGS) && !(formats & CRASH_FMT_VISUAL_PANES))
        log_dump_panic();

    if (depth == 0)
        spin_unlock_raw(&crash_lock);

#if defined(TEST_ENABLED) || defined(TEST_NIGHTMARE_ENABLED)
    ndjson_bye(QEMU_EXIT_PANIC, "crash");
    qemu_exit(QEMU_EXIT_PANIC);
#endif

    while (true)
        wait_for_interrupt();
}
