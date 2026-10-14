/* @title: Logging */
#pragma once
#include <bootstage.h>
#include <colors.h>
#include <compiler/diagnostic.h>
#include <compiler/intrinsic.h>
#include <linker/symbols.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <types/refcount.h>
#include <types/types.h>

#define LOG_NARGS 10
struct log_site;
struct log_record;

enum log_handle_flags : uint32_t {
    LOG_HANDLE_PRINT = 1 << 0,     /* emit to console immediately */
    LOG_HANDLE_IMPORTANT = 1 << 1, /* never drop / elevated visibility */
    LOG_HANDLE_RATELIMIT = 1 << 2, /* suppress floods */
    LOG_HANDLE_ONCE = 1 << 3,      /* print only first occurrence */
    LOG_HANDLE_PANIC = 1 << 4,     /* fatal if level >= ERROR */
    LOG_HANDLE_NO_NEWLINE = 1 << 5,
    LOG_HANDLE_FLAGS_DEFAULT = 0,
};

enum log_level : uint8_t {
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

enum log_site_flags : uint32_t {
    LOG_SITE_PRINT = 1 << 0,    /* print all logs in site */
    LOG_SITE_DROP_OLD = 1 << 1, /* overwrite oldest on overflow */
    LOG_SITE_NO_IRQ = 1 << 2,   /* suppress in IRQ context */
    LOG_SITE_PANIC_VISIBLE = 1 << 3,

    LOG_SITE_NDJSON = 1 << 4, /* emit records directly to NDJSON wire */
    LOG_SITE_NONE = 0,
    LOG_SITE_DEFAULT = LOG_SITE_DROP_OLD,
};

enum log_record_flags : uint16_t {
    LOG_REC_FROM_IRQ = 1 << 0,
    LOG_REC_TRUNCATED = 1 << 1,
};

/* Governs what to print */
struct log_dump_options {
    uint8_t min_level;
    bool show_cpu : 1;
    bool show_tid : 1;
    bool show_irql : 1;
    bool show_caller : 1;
    bool resolve_symbols : 1;
    bool clear_after_dump : 1;
};

struct log_handle {
    void (*print)(const struct log_site *site, const struct log_record *rec,
                  void (*print)(const char *fmt, ...) cc_printf_like(1, 2));
    enum log_handle_flags flags;
    atomic_uint32_t seen_internal;
    atomic_uint64_t last_ts_internal;
};

struct log_record {
    time_ms_t timestamp;
    cpu_id_t cpu;
    uint32_t tid;

    const struct log_handle *handle;
    enum log_level level;
    enum bootstage boostage;

    uint16_t msg_len;

    char *fmt; /* Becomes a pointer to the shadow_buf in cases where
                * LOG_SITE_DUP_MESSAGES is set */
    uint8_t nargs;
    uint64_t args[LOG_NARGS];

    char *caller_fn;
    char *caller_file;
    int32_t caller_line;
    uintptr_t caller_pc;
    enum log_record_flags flags;
    enum irql logged_at_irql;
};

struct log_ring_slot {
    atomic_uint64_t seq;
    struct log_record rec;
    char *shadow_buf; /* With a len of msg_max_len */
};

struct log_ringbuf {
    struct log_ring_slot *slots;

    atomic_uint64_t head;
    atomic_uint64_t tail;
};

/* Ephemeral argument passed in creation */
struct log_site_options {
    char *name;
    enum log_site_flags flags;
    size_t capacity;
    size_t msg_max_len; /* only if LOG_SITE_DUP_MESSAGES on,
                         * will truncate trailing characters if strlen
                         * of the input fmt str is too long */

    uint32_t enabled_mask;
    struct log_dump_options dump_opts;
};

struct log_site {
    struct list_node list;
    char *name;
    struct log_ringbuf rb;
    uint32_t enabled_mask; /* Governs what actually goes in the site */
    size_t capacity;

    /* Only relevant for dynamic log sites */
    refcount_t refcount;

    size_t msg_max_len;
    uint32_t dropped; /* Accumulation of all missed logs */
    enum log_site_flags flags;
    struct log_dump_options dump_opts; /* If LOG_SITE_PRINT, use this */
};

static inline const char *log_level_color(enum log_level l) {
    switch (l) {
    case LOG_INFO: return ANSI_GREEN;
    case LOG_WARN: return ANSI_YELLOW;
    case LOG_ERROR: return ANSI_RED;
    case LOG_TRACE: return ANSI_BLUE;
    case LOG_DEBUG: return ANSI_MAGENTA;
    default: return ANSI_RESET;
    }
}

static inline bool log_handle_should_print(const struct log_handle *h,
                                           const struct log_site *s,
                                           uint8_t level) {
    if (h->flags & LOG_HANDLE_PRINT)
        return true;

    if (s && (s->flags & LOG_SITE_PRINT))
        return true;

    if ((h->flags & LOG_HANDLE_PANIC) && level >= LOG_ERROR)
        return true;

    return false;
}

static inline bool log_site_enabled(const struct log_site *ss, uint8_t level) {
    if (!ss)
        return true;

    return ss->enabled_mask & (1u << level);
}

void log_emit_full(struct log_site *, struct log_handle *, enum log_level,
                   const char *func, const char *fname, int32_t line,
                   uintptr_t ip, uint8_t nargs, char *fmt, ...)
    cc_printf_like(9, 10);
void log_dump_site_with_opts(struct log_site *, struct log_dump_options opts);
void log_dump_site(struct log_site *site);
void log_dump_site_default(struct log_site *);
void log_dump_all(void);
void log_dump_panic(void);
void log_sites_init(void);
void log_site_free(struct log_site *site);
struct log_site *log_site_create(struct log_site_options opts);
void log_handle_init(struct log_handle *h, enum log_handle_flags f);

static inline bool log_site_getref(struct log_site *site) {
    return refcount_inc_not_zero(&site->refcount);
}

static inline void log_site_putref(struct log_site *site) {
    if (refcount_dec_and_test(&site->refcount))
        log_site_free(site);
}

/* NOTE: RACY, only usable as a heuristic */
static inline size_t log_site_message_count(struct log_site *site) {
    uint64_t head = atomic_load_acq(&site->rb.head);
    uint64_t tail = atomic_load_acq(&site->rb.tail);

    return head - tail;
}

#define LOG_HANDLE_DEFAULT                                                     \
    (struct log_handle){.flags = LOG_HANDLE_FLAGS_DEFAULT,                     \
                        .seen_internal = 0,                                    \
                        .last_ts_internal = 0}

#define LOG_DUMP_DEFAULT                                                       \
    (struct log_dump_options) {                                                \
        .min_level = LOG_TRACE, .show_cpu = true, .show_tid = true,            \
        .show_irql = true, .show_caller = true, .resolve_symbols = true,       \
        .clear_after_dump = false,                                             \
    }

#define LOG_DUMP_CONSOLE                                                       \
    (struct log_dump_options) {                                                \
        .min_level = LOG_TRACE, .show_cpu = false, .show_tid = false,          \
        .show_irql = false, .show_caller = false, .resolve_symbols = false,    \
        .clear_after_dump = false,                                             \
    }

#define log(site, handle, lvl, fmt, ...)                                       \
    log_emit_full(site, handle, lvl, __func__, __FILE__, __LINE__,             \
                  (uintptr_t) ci_return_address(0), PP_NARG(__VA_ARGS__), fmt, \
                  ##__VA_ARGS__)

#define log_global(handle, lvl, fmt, ...)                                      \
    log_emit_full(LOG_SITE(global), handle, lvl, __func__, __FILE__, __LINE__, \
                  (uintptr_t) ci_return_address(0), PP_NARG(__VA_ARGS__), fmt, \
                  ##__VA_ARGS__)

#define log_msg(lvl, fmt, ...)                                                 \
    log_emit_full(LOG_SITE(global), LOG_HANDLE(global), lvl, __func__,         \
                  __FILE__, __LINE__, (uintptr_t) ci_return_address(0),        \
                  PP_NARG(__VA_ARGS__), fmt, ##__VA_ARGS__)

#define log_once(site, handle, lvl, fmt, ...)                                  \
    do {                                                                       \
        static bool done_internal_log = false;                                 \
        if (!done_internal_log) {                                              \
            log(site, handle, lvl, fmt, ##__VA_ARGS__);                        \
            done_internal_log = true;                                          \
        }                                                                      \
    } while (0)

#define log_global_once(handle, lvl, fmt, ...)                                 \
    log_once(LOG_SITE(global), handle, lvl, fmt, ##__VA_ARGS__)

#define log_msg_once(lvl, fmt, ...)                                            \
    log_global_once(LOG_HANDLE(global), lvl, fmt, ##__VA_ARGS__)

#define log_err_global(handle, fmt, ...)                                       \
    log_global(handle, LOG_ERROR, fmt, ##__VA_ARGS__)
#define log_warn_global(handle, fmt, ...)                                      \
    log_global(handle, LOG_WARN, fmt, ##__VA_ARGS__)
#define log_info_global(handle, fmt, ...)                                      \
    log_global(handle, LOG_INFO, fmt, ##__VA_ARGS__)
#define log_debug_global(handle, fmt, ...)                                     \
    log_global(handle, LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_trace_global(handle, fmt, ...)                                     \
    log_global(handle, LOG_TRACE, fmt, ##__VA_ARGS__)

#define LOG_SITE_CAPACITY_DEFAULT 128 /* good enough for most purposes */
#define LOG_SITE_EXTERN(name) extern struct log_site __log_site_##name

/* For static ones */
#define LOG_SITE_LEVEL(l) (1u << l)
#define LOG_SITE_ALL UINT32_MAX

#define LOG_SITE_DEFINE(_name, ...)                                            \
    cc_wno_override_init_start LINKER_SECTION_ATTRIBUTE(log_sites)             \
    struct log_site __log_site_##_name = {.capacity =                          \
                                              LOG_SITE_CAPACITY_DEFAULT,       \
                                          .enabled_mask = LOG_SITE_ALL,        \
                                          .dump_opts = LOG_DUMP_CONSOLE,       \
                                          .name = #_name,                      \
                                          __VA_ARGS__};                        \
    cc_wno_override_init_end

#define LOG_SITE_DEFINE_PRINT(_name, ...)                                      \
    cc_wno_override_init_start LINKER_SECTION_ATTRIBUTE(log_sites)             \
    struct log_site __log_site_##_name = {                                     \
        .name = #_name,                                                        \
        .flags = LOG_SITE_DEFAULT,                                             \
        .capacity = LOG_SITE_CAPACITY_DEFAULT,                                 \
        .enabled_mask = LOG_SITE_ALL,                                          \
        .dump_opts = LOG_DUMP_CONSOLE,                                         \
        __VA_ARGS__}; /* Rest will get initialized at boot */                  \
    cc_wno_override_init_end

#define LOG_SITE(name) (&(__log_site_##name))

#define LOG_HANDLE_SUBSYSTEM_NONE NULL
#define LOG_HANDLE_EXTERN(name) extern struct log_handle __log_handle_##name

#define LOG_HANDLE_DEFINE(_name, ...)                                          \
    cc_wno_override_init_start struct log_handle __log_handle_##_name = {      \
        .seen_internal = 0, .last_ts_internal = 0, __VA_ARGS__};               \
    cc_wno_override_init_end

#define _LOG_HANDLE_DEFINE_PRINT(n, storage, ...)                              \
    storage struct log_handle __log_handle_##n = {.flags = LOG_HANDLE_PRINT,   \
                                                  .seen_internal = 0,          \
                                                  .last_ts_internal = 0,       \
                                                  __VA_ARGS__}

#define LOG_HANDLE_DEFINE_PRINT(n, ...)                                        \
    cc_wno_override_init_start _LOG_HANDLE_DEFINE_PRINT(n, , __VA_ARGS__);     \
    cc_wno_override_init_end

#define LOG_HANDLE_DEFINE_PRINT_STATIC(n, ...)                                 \
    cc_wno_override_init_start _LOG_HANDLE_DEFINE_PRINT(n, static,             \
                                                        __VA_ARGS__);          \
    cc_wno_override_init_end

#define LOG_HANDLE(name) &(__log_handle_##name)

LINKER_SECTION_EXTERN(struct log_site, log_sites);

LOG_HANDLE_EXTERN(global);
LOG_SITE_EXTERN(global);
