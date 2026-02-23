/* @title: Logging */
#pragma once
#include <colors.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <types/types.h>

enum log_flags : uint32_t {
    LOG_PRINT = 1 << 0,     /* emit to console immediately */
    LOG_IMPORTANT = 1 << 1, /* never drop / elevated visibility */
    LOG_RATELIMIT = 1 << 2, /* suppress floods */
    LOG_ONCE = 1 << 3,      /* print only first occurrence */
    LOG_PANIC = 1 << 5,     /* fatal if level >= ERROR */
    LOG_DEFAULT = LOG_PRINT,
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
    LOG_SITE_NONE = 0,
    LOG_SITE_DEFAULT = LOG_SITE_DROP_OLD,
};

enum log_record_flags : uint16_t {
    LOG_REC_FROM_IRQ = 1 << 0,
    LOG_REC_TRUNCATED = 1 << 1,
};

struct log_dump_opts {
    uint8_t min_level;
    bool show_args : 1;
    bool show_cpu : 1;
    bool show_tid : 1;
    bool show_irql : 1;
    bool show_caller : 1;
    bool resolve_symbols : 1;
    bool clear_after_dump : 1;
};

struct log_handle {
    const char *msg;
    enum log_flags flags;
    _Atomic uint32_t seen;
    _Atomic uint64_t last_ts;
    struct log_dump_opts dump_opts; /* If LOG_PRINT, use these opts */
};

struct log_record {
    time_t timestamp;
    cpu_id_t cpu;
    uint32_t tid;

    const struct log_handle *handle;
    enum log_level level;

    uint16_t msg_len;

    const char *fmt;
    uint8_t nargs;
    uint64_t args[8];

    char *caller_fn;
    char *caller_file;
    int32_t caller_line;
    uintptr_t caller_pc;
    enum log_record_flags flags;
    enum irql logged_at_irql;
};

struct log_ring_slot {
    _Atomic uint64_t seq;
    struct log_record rec;
};

struct log_ringbuf {
    size_t capacity;
    struct log_ring_slot *slots;

    _Atomic uint64_t head;
    _Atomic uint64_t tail;
};

struct log_site {
    struct list_head list;
    char *name;
    struct log_ringbuf rb;
    uint32_t enabled_mask;

    /* Only relevant for dynamic log sites */
    refcount_t refcount;
    atomic_bool enabled;

    uint32_t dropped; /* Accumulation of all missed logs */
    enum log_site_flags flags;
    struct log_dump_opts dump_opts; /* If LOG_SITE_PRINT, use this */
} __linker_aligned;

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
    if (h->flags & LOG_PRINT)
        return true;

    if (s && (s->flags & LOG_SITE_PRINT))
        return true;

    if ((h->flags & LOG_PANIC) && level >= LOG_ERROR)
        return true;

    return false;
}

static inline bool log_site_accepts(struct log_site *s) {
    return atomic_load_explicit(&s->enabled, memory_order_relaxed);
}

static inline uint64_t log_arg_u64(uint64_t v) {
    return v;
}

static inline uint64_t log_arg_ptr(const void *p) {
    return (uintptr_t) p;
}

static inline bool log_site_enabled(const struct log_site *ss, uint8_t level) {
    if (!ss)
        return true;

    return ss->enabled_mask & (1u << level);
}

static inline uint64_t log_arg_i64(int64_t v) {
    return (uint64_t) v;
}

void log_emit_internal(struct log_site *, struct log_handle *, enum log_level,
                       const char *func, const char *fname, int32_t line,
                       uintptr_t ip, uint8_t nargs, char *fmt, ...);
void log_dump_site(struct log_site *, struct log_dump_opts opts);
void log_dump_site_default(struct log_site *);
void log_dump_all(void);
void log_sites_init(void);
struct log_site *log_site_create(char *name, enum log_site_flags flags,
                                 size_t capacity);
void log_site_destroy(struct log_site *site);

#define LOG_DUMP_DEFAULT                                                       \
    (struct log_dump_opts) {                                                   \
        .min_level = LOG_TRACE, .show_args = true, .show_cpu = true,           \
        .show_tid = true, .show_irql = true, .show_caller = true,              \
        .resolve_symbols = true, .clear_after_dump = false,                    \
    }

#define LOG_DUMP_CONSOLE                                                       \
    (struct log_dump_opts) {                                                   \
        .min_level = LOG_TRACE, .show_args = true, .show_cpu = false,          \
        .show_tid = false, .show_irql = false, .show_caller = false,           \
        .resolve_symbols = false, .clear_after_dump = false,                   \
    }

#define log_msg(lvl, fmt, ...)                                                 \
    log_emit_internal(LOG_SITE(global), LOG_HANDLE(global), lvl, __func__,     \
                      __FILE__, __LINE__,                                      \
                      (uintptr_t) __builtin_return_address(0),                 \
                      PP_NARG(__VA_ARGS__), fmt, ##__VA_ARGS__)

#define log_global(handle, lvl, fmt, ...)                                      \
    log_emit_internal(LOG_SITE(global), handle, lvl, __func__, __FILE__,       \
                      __LINE__, (uintptr_t) __builtin_return_address(0),       \
                      PP_NARG(__VA_ARGS__), fmt, ##__VA_ARGS__)

#define log(site, handle, lvl, fmt, ...)                                       \
    log_emit_internal(site, handle, lvl, __func__, __FILE__, __LINE__,         \
                      (uintptr_t) __builtin_return_address(0),                 \
                      PP_NARG(__VA_ARGS__), fmt, ##__VA_ARGS__)

#define log_err(site, handle, fmt, ...)                                        \
    log(site, handle, LOG_ERROR, fmt, ##__VA_ARGS__)
#define log_warn(site, handle, fmt, ...)                                       \
    log(site, handle, LOG_WARN, fmt, ##__VA_ARGS__)
#define log_info(site, handle, fmt, ...)                                       \
    log(site, handle, LOG_INFO, fmt, ##__VA_ARGS__)
#define log_debug(site, handle, fmt, ...)                                      \
    log(site, handle, LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_trace(site, handle, fmt, ...)                                      \
    log(site, handle, LOG_TRACE, fmt, ##__VA_ARGS__)

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
#define LOG_SITE_ALL UINT32_MAX /* TODO: More */
#define LOG_SITE_DECLARE(n, f, size, mask, dopts)                              \
    __attribute__((                                                            \
        section(".kernel_log_sites"))) struct log_site __log_site_##n =        \
        (struct log_site) {                                                    \
        .name = #n, .flags = f, .rb.capacity = size, .enabled_mask = mask,     \
        .dump_opts = dopts                                                     \
    } /* Rest will get initialized at boot */
#define LOG_SITE_DECLARE_DEFAULT(n)                                            \
    LOG_SITE_DECLARE(n, LOG_SITE_DEFAULT, LOG_SITE_CAPACITY_DEFAULT,           \
                     LOG_SITE_ALL, LOG_DUMP_CONSOLE)

#define LOG_SITE(name) &(__log_site_##name)

#define LOG_HANDLE_SUBSYSTEM_NONE NULL
#define LOG_HANDLE_EXTERN(name) extern struct log_handle __log_handle_##name
#define LOG_HANDLE_DECLARE(na, f, dopts)                                       \
    struct log_handle __log_handle_##na = (struct log_handle) {                \
        .msg = #na, .flags = f, .seen = 0, .last_ts = 0, .dump_opts = dopts    \
    }

#define LOG_HANDLE_DECLARE_DEFAULT(n)                                          \
    LOG_HANDLE_DECLARE(n, LOG_PRINT, LOG_DUMP_CONSOLE)

#define LOG_HANDLE(name) &(__log_handle_##name)

extern struct log_site __skernel_log_sites[];
extern struct log_site __ekernel_log_sites[];

LOG_HANDLE_EXTERN(global);
LOG_SITE_EXTERN(global);
