/* @title: Nightmare test harness */
#pragma once
#include <cmdline.h>
#include <compiler.h>
#include <kassert.h>
#include <linker/symbols.h>
#include <math/fixed.h>
#include <scaled_param.h>
#include <smp/percpu.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types/types.h>

struct nightmare_ctx;
struct nightmare_worker;
struct thread;

#define NIGHTMARE_EXIT_OK 0
#define NIGHTMARE_EXIT_FINDING 3
#define NIGHTMARE_EXIT_FAIL 4
#define NIGHTMARE_EXIT_STALL 5
#define NIGHTMARE_EXIT_SKIP 6

#define NIGHTMARE_INTENSITY_SENTINEL ((fx32_32_t) - 1LL)
#define NIGHTMARE_INTENSITY_DEFAULT FX(0.5)

enum nightmare_seed_policy : uint8_t {
    NIGHTMARE_SEED_IGNORED = 0,
    NIGHTMARE_SEED_OPTIONAL,
    NIGHTMARE_SEED_REQUIRED,
};

enum nightmare_seed_mode : uint8_t {
    NIGHTMARE_SEED_SPLIT = 0,
    NIGHTMARE_SEED_SEEDFUL,
    NIGHTMARE_SEED_SEEDLESS,
};

enum nightmare_requires : uint32_t {
    NIGHTMARE_REQ_NONE = 0,
    NIGHTMARE_REQ_SMP = 1 << 0,
    NIGHTMARE_REQ_PREEMPT = 1 << 1,
    NIGHTMARE_REQ_ASAN = 1 << 2,
    NIGHTMARE_REQ_INJECT = 1 << 3,
};

enum nightmare_result : uint8_t {
    NIGHTMARE_RESULT_OK = 0,
    NIGHTMARE_RESULT_FINDING,
    NIGHTMARE_RESULT_FAIL,
    NIGHTMARE_RESULT_STALL,
    NIGHTMARE_RESULT_SKIP,
};

enum nightmare_skip_reason : uint8_t {
    NIGHTMARE_SKIP_NONE = 0,
    NIGHTMARE_SKIP_NOT_COMPILED,
    NIGHTMARE_SKIP_NO_SUCH_NIGHTMARE,
    NIGHTMARE_SKIP_NEEDS_SMP,
    NIGHTMARE_SKIP_NEEDS_PREEMPT,
    NIGHTMARE_SKIP_NEEDS_ASAN,
    NIGHTMARE_SKIP_NEEDS_INJECT,
    NIGHTMARE_SKIP_RAM_LOW,
    NIGHTMARE_SKIP_SEED_UNUSED,
    NIGHTMARE_SKIP_SEED_MISSING,
    NIGHTMARE_SKIP_SERVICE_MISSING,
    NIGHTMARE_SKIP_PREPARE_REFUSED,
};

enum nightmare_stop : uint8_t {
    NM_RUN = 0,
    NM_STOP_BUDGET,
    NM_STOP_FAIL,
    NM_STOP_STALL,
};

enum nightmare_finding_tier : uint8_t {
    NIGHTMARE_TIER_AMBIGUOUS = 0,
    NIGHTMARE_TIER_CONFIDENT,
};

struct nightmare_verdict {
    enum nightmare_result result;
    enum nightmare_skip_reason skip_reason;
    const char *reason;
    const char *msg;
};

#define NIGHTMARE_OK                                                           \
    ((struct nightmare_verdict) {.result = NIGHTMARE_RESULT_OK})
#define NIGHTMARE_FAIL(reason_, msg_)                                          \
    ((struct nightmare_verdict) {                                              \
        .result = NIGHTMARE_RESULT_FAIL, .reason = (reason_), .msg = (msg_)})
#define NIGHTMARE_SKIP(reason_)                                                \
    ((struct nightmare_verdict) {.result = NIGHTMARE_RESULT_SKIP,              \
                                 .skip_reason = (reason_)})

struct nightmare_rng {
    uint64_t state;
};

struct nightmare_worker {
    size_t index;
    const char *role;
    struct nightmare_rng rng;
    _Atomic(struct thread *) th;
    atomic_bool parked;
};

struct nightmare_ctx {
    const struct nightmare *nm;
    fx32_32_t intensity;
    size_t intensity_val;
    size_t worker_count;
    uint64_t seed;
    bool seed_present;
    enum nightmare_seed_mode seed_mode;
    time_ms_t soft_deadline_ms;
    time_ms_t hard_deadline_ms;
    void *private;
};

struct nightmare_ops {
    struct nightmare_verdict (*prepare)(struct nightmare_ctx *);
    void (*worker)(struct nightmare_ctx *, struct nightmare_worker *);
    struct nightmare_verdict (*quiesce_check)(struct nightmare_ctx *);
    void (*probe)(struct nightmare_ctx *);
    struct nightmare_verdict (*finish)(struct nightmare_ctx *);
};

struct nightmare {
    const char *name;
    const char *fname;
    const char *desc;
    const struct nightmare_ops *ops;
    enum nightmare_seed_policy seed_policy;
    enum nightmare_requires requires;
    const char *const *perturb;
    fx32_32_t intensity;
    struct scaled_param intensity_desc;
    time_ms_t default_duration_ms;
    size_t min_mem_mib;
} __aligned(8);

LINKER_SECTION_DEFINE(struct nightmare, nightmares);

#define NIGHTMARE_DECLARE(id, ...)                                             \
    extern struct nightmare __nightmare_##id;                                  \
    LINKER_SECTION_OBJECT(struct nightmare, nightmares)                        \
    __nightmare_##id = {.name = #id,                                           \
                        .fname = __RELFILE__,                                  \
                        .seed_policy = NIGHTMARE_SEED_IGNORED,                 \
                        .requires = NIGHTMARE_REQ_NONE,                        \
                        .intensity = NIGHTMARE_INTENSITY_SENTINEL,             \
                        __VA_ARGS__}

#define NIGHTMARE(id) (&__nightmare_##id)
#define NIGHTMARE_DEFINE(id) extern struct nightmare __nightmare_##id
#define NIGHTMARE_PERTURB(...) ((const char *const[]) {__VA_ARGS__, NULL})

#define NIGHTMARE_INTENSITY_CORES(min_, def_, max_, unit_)                     \
    .intensity_desc = {.curve = SCALE_CORE_MULTIPLIER,                         \
                       .min_val = (min_),                                      \
                       .def_val = (def_),                                      \
                       .max_val = (max_),                                      \
                       .unit = (unit_)}

#define NIGHTMARE_WORKER(id)                                                   \
    static void id(struct nightmare_ctx *NM_CTX,                               \
                   struct nightmare_worker *NM_SELF)

#define NIGHTMARE_OPTIONS_DECLARE(id, struct_type, instance, ...)              \
    static void *__nightmare_options_resolve_##id(const char *path,            \
                                                  size_t path_len) {           \
        return path_len == sizeof(#id) - 1 &&                                  \
                       strncmp(path, #id, path_len) == 0                       \
                   ? &(instance)                                               \
                   : NULL;                                                     \
    }                                                                          \
    CMDLINE_SCHEMA_DECLARE(__nightmare_options_##id, "nightmare", #id,         \
                           "Nightmare subject options",                        \
                           __nightmare_options_resolve_##id, __VA_ARGS__)

struct nightmare_progress_counter {
    _Atomic uint64_t count;
};

PERCPU_DEFINE(nightmare_progress, struct nightmare_progress_counter);

static inline void nightmare_progress_tick(void) {
    atomic_fetch_add_explicit(&PERCPU_PTR(nightmare_progress)->count, 1,
                              memory_order_relaxed);
}

#define NIGHTMARE_PROGRESS() nightmare_progress_tick()

struct nightmare_finding_site {
    const char *kind;
    enum nightmare_finding_tier tier;
    const char *file;
    uint32_t line;
};

#define NIGHTMARE_FINDING(kind_, fmt, ...)                                     \
    nightmare_finding_at(                                                      \
        &(const struct nightmare_finding_site) {.kind = (kind_),               \
                                                .tier =                        \
                                                    NIGHTMARE_TIER_AMBIGUOUS,  \
                                                .file = __RELFILE__,           \
                                                .line = __LINE__},             \
        0, (fmt), ##__VA_ARGS__)

#define NIGHTMARE_FINDING_TIER(kind_, tier_, discriminator_, fmt, ...)         \
    nightmare_finding_at(                                                      \
        &(const struct nightmare_finding_site) {.kind = (kind_),               \
                                                .tier = (tier_),               \
                                                .file = __RELFILE__,           \
                                                .line = __LINE__},             \
        (discriminator_), (fmt), ##__VA_ARGS__)

struct nightmare_stall_evidence {
    cpu_id_t cpu;
    thread_id_t tid;
    size_t worker_index;
    const char *role;
    time_ms_t last_progress_ms;
    uint64_t progress;
};

void nightmare_run(void);
bool nightmare_must_stop(void);
bool nightmare_must_stop_irq(void);
bool nightmare_must_park(void);
void nightmare_park(struct nightmare_worker *worker);
uint64_t nightmare_rand(struct nightmare_rng *rng);
uint64_t nightmare_progress_sum_irq(void);
void nightmare_finding_at(const struct nightmare_finding_site *site,
                          uint64_t discriminator, const char *fmt, ...)
    __printf_like(3, 4);
void nightmare_request_external_fail(const char *kind, uint64_t discriminator,
                                     const char *fmt, ...) __printf_like(3, 4);
void nightmare_report_stall(const struct nightmare_stall_evidence *evidence);
