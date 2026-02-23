/* @title: Logging */
#pragma once
#include <colors.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/locked_list.h>
#include <sync/spinlock.h>
#include <types/types.h>

enum log_event_flags : uint32_t {
    LOG_EVENT_PRINT = 1 << 0,     /* emit to console immediately */
    LOG_EVENT_IMPORTANT = 1 << 1, /* never drop / elevated visibility */
    LOG_EVENT_RATELIMIT = 1 << 2, /* suppress floods */
    LOG_EVENT_ONCE = 1 << 3,      /* print only first occurrence */
    LOG_EVENT_TRACE = 1 << 4,     /* eligible for tracing backend */
    LOG_EVENT_PANIC = 1 << 5,     /* fatal if level >= ERROR */
};

enum log_level : uint8_t {
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

enum log_site_flags : uint32_t {
    LOG_SITE_PRINT = 1 << 0, /* print all events in site */
    LOG_SITE_PERSIST = 1 << 1,
    LOG_SITE_DROP_OLD = 1 << 2, /* overwrite oldest on overflow */
    LOG_SITE_NO_IRQ = 1 << 3,   /* suppress in IRQ context */
    LOG_SITE_NONE = 0,
};

enum log_record_flags : uint16_t {
    LOG_REC_FROM_IRQ = 1 << 0,
    LOG_REC_TRUNCATED = 1 << 1,
};

enum log_site_type {
    LOG_SITE_TYPE_STATIC,
    LOG_SITE_TYPE_DYNAMIC,
    LOG_SITE_TYPE_EPHEMERAL,
};

struct log_subsystem {
    const char *name;
    uint32_t enabled_mask;
};

struct log_event_handle {
    const char *msg;
    struct log_subsystem *subsystem;
    enum log_level level;
    enum log_event_flags flags;
    _Atomic uint32_t seen;
    _Atomic uint64_t last_ts;
    void *src;
};

struct log_event_record {
    time_t timestamp;
    cpu_id_t cpu;
    uint32_t tid;

    const struct log_event_handle *handle;
    enum log_level level;

    uint16_t msg_len;

    const char *fmt;
    uint8_t nargs;
    uint64_t args[8];

    vaddr_t caller_ip;
    enum log_record_flags flags;
    enum irql logged_at_irql;
};

struct log_ring_slot {
    _Atomic uint64_t seq;
    struct log_event_record rec;
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

    /* Only relevant for dynamic log sites */
    refcount_t refcount;
    atomic_bool enabled;

    uint32_t dropped; /* Accumulation of all missed logs */
    enum log_site_flags flags;
} __linker_aligned;

struct log_globals {
    struct locked_list list;
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

static inline uint8_t log_event_level(const struct log_event_handle *h) {
    return h->level;
}

static inline bool log_event_should_print(const struct log_event_handle *h,
                                          const struct log_site *s,
                                          uint8_t level) {
    if (h->flags & LOG_EVENT_PRINT)
        return true;

    if (s && (s->flags & LOG_SITE_PRINT))
        return true;

    if ((h->flags & LOG_EVENT_PANIC) && level >= LOG_ERROR)
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

static inline bool log_subsystem_enabled(const struct log_subsystem *ss,
                                         uint8_t level) {
    if (!ss)
        return true;

    return ss->enabled_mask & (1u << level);
}

static inline uint64_t log_arg_i64(int64_t v) {
    return (uint64_t) v;
}

void log_emit(struct log_site *, struct log_event_handle *, uint8_t nargs,
              char *fmt, ...);

void log_dump_site(struct log_site *, struct log_dump_opts *opts);
void log_dump_site_default(struct log_site *);
void log_dump_all(void);
void log_sites_init(void);
void log_set_subsystem_level(struct log_subsystem *, uint8_t mask);
struct log_site *log_site_create(char *name, enum log_site_flags flags,
                                 size_t capacity);
void log_site_destroy(struct log_site *site);

#define k_log(site, handle, fmt, ...)                                          \
    log_emit(site, handle, PP_NARG(__VA_ARGS__), fmt, ##__VA_ARGS__)

#define k_log_lvl(site, handle, lvl, fmt, ...)                                 \
    log_emit_lvl(site, handle, lvl, PP_NARG(__VA_ARGS__), fmt, ##__VA_ARGS__)

#define k_log_err(site, handle, fmt, ...)                                      \
    k_log_lvl(site, handle, LOG_ERROR, fmt, ##__VA_ARGS__)

#define LOG_SITE_DECLARE(name)                                                 \
    __attribute__((section(".kernel_log_sites"))) struct log_site name

extern struct log_site __skernel_log_sites[];
extern struct log_site __ekernel_log_sites[];

static struct log_dump_opts log_dump_default = {
    .min_level = LOG_TRACE,
    .show_args = true,
    .show_cpu = true,
    .show_tid = true,
    .show_irql = true,
    .show_caller = true,
    .resolve_symbols = true,
    .clear_after_dump = false,
};
