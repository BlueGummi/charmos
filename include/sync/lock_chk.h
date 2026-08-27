/* @title: Lock Validation */
#pragma once
#include <compiler.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lock_chk_class;

enum lock_chk_flags : uint8_t {
    LOCK_UNCHKD = 0,
    LOCK_CHKD_ORDER = 1 << 0,
    LOCK_CHKD_THREAD = 1 << 1,
    LOCK_CHKD_FULL = LOCK_CHKD_ORDER | LOCK_CHKD_THREAD,
};

enum lock_chk_type : uint8_t {
    LOCK_CHK_TYPE_SPIN,
    LOCK_CHK_TYPE_QSPIN,
    LOCK_CHK_TYPE_MUTEX,
    LOCK_CHK_TYPE_MUTEX_SIMPLE,
    LOCK_CHK_TYPE_RWLOCK,
};

enum lock_chk_mode : uint8_t {
    LOCK_CHK_MODE_SHARED,
    LOCK_CHK_MODE_EXCLUSIVE,
};

enum lock_chk_wait_kind : uint8_t {
    LOCK_CHK_WAIT_BLOCKING,
    LOCK_CHK_WAIT_TRY,
};

enum lock_chk_engine_state : uint8_t {
    LOCK_CHK_INACTIVE,
    LOCK_CHK_ACTIVE,
    LOCK_CHK_DEGRADED,
};

enum lock_debug_irq_usage : uint8_t {
    LOCK_DEBUG_IRQ_NONE,
    LOCK_DEBUG_IRQ_DISPATCH,
    LOCK_DEBUG_IRQ_HIGH,
};

struct lock_chk_site {
    const char *file;
    const char *func;
    uint32_t line;
};

struct lock_chk_class {
    const char *name;
    const char *file;
    uint32_t line;
};

#ifdef DEBUG_LOCK_CHK

#define LOCK_CHK_SITE_HERE()                                                   \
    ({                                                                         \
        static const struct lock_chk_site __site = {                           \
            .file = __RELFILE__,                                               \
            .func = __func__,                                                  \
            .line = __LINE__,                                                  \
        };                                                                     \
        &__site;                                                               \
    })

#define LOCK_CHK_CLASS(id) (&__lock_chk_class_##id)

#define LOCK_CHK_CLASS_DECLARE(id)                                             \
    extern const struct lock_chk_class __lock_chk_class_##id;                  \
    const struct lock_chk_class __lock_chk_class_##id = {                      \
        .name = #id,                                                           \
        .file = __RELFILE__,                                                   \
        .line = __LINE__,                                                      \
    }

#define LOCK_CHK_CLASS_DECLARE_LOCAL(id)                                       \
    static const struct lock_chk_class __lock_chk_class_##id = {               \
        .name = #id,                                                           \
        .file = __RELFILE__,                                                   \
        .line = __LINE__,                                                      \
    }

#define LOCK_CHK_CLASS_DEFINE(id)                                              \
    extern const struct lock_chk_class __lock_chk_class_##id

#else /* !defined(DEBUG_LOCK_CHK) */

#define LOCK_CHK_SITE_HERE() ((const struct lock_chk_site *) NULL)
#define LOCK_CHK_CLASS(id) ((const struct lock_chk_class *) NULL)
#define LOCK_CHK_CLASS_DECLARE(id)
#define LOCK_CHK_CLASS_DECLARE_LOCAL(id)
#define LOCK_CHK_CLASS_DEFINE(id)

#endif /* DEBUG_LOCK_CHK */

/*
 * Prototypes
 */
#ifdef DEBUG_LOCK_CHK

void lock_debug_spin_classify(_Atomic uint8_t *usage,
                              enum lock_debug_irq_usage requested,
                              void *instance, enum lock_chk_type type,
                              const struct lock_chk_site *site);
bool lock_debug_spin_push(void *instance, enum lock_chk_type type,
                          enum irql prev_irql,
                          const struct lock_chk_site *site);
void lock_debug_spin_validate_top(void *instance, enum lock_chk_type type,
                                  enum irql prev_irql,
                                  const struct lock_chk_site *site);
void lock_debug_spin_pop(void *instance, enum lock_chk_type type);

void lock_chk_note_lock_use(bool initialized, enum lock_chk_flags flags,
                            _Atomic bool *used, bool manages_irql,
                            bool raw_operation);

void lock_chk_assert_schedulable(const struct lock_chk_site *site);

#else /* !defined(DEBUG_LOCK_CHK) */

static inline void lock_debug_spin_classify(_Atomic uint8_t *usage,
                                            enum lock_debug_irq_usage requested,
                                            void *instance,
                                            enum lock_chk_type type,
                                            const struct lock_chk_site *site) {
    unused(usage, requested, instance, type, site);
}

static inline bool lock_debug_spin_push(void *instance, enum lock_chk_type type,
                                        enum irql prev_irql,
                                        const struct lock_chk_site *site) {
    unused(instance, type, prev_irql, site);
    return false;
}

static inline void
lock_debug_spin_validate_top(void *instance, enum lock_chk_type type,
                             enum irql prev_irql,
                             const struct lock_chk_site *site) {
    unused(instance, type, prev_irql, site);
}

static inline void lock_debug_spin_pop(void *instance,
                                       enum lock_chk_type type) {
    unused(instance, type);
}

static inline void lock_chk_note_lock_use(bool initialized,
                                          enum lock_chk_flags flags,
                                          _Atomic bool *used, bool manages_irql,
                                          bool raw_operation) {
    unused(initialized, flags, used, manages_irql, raw_operation);
}

static inline void
lock_chk_assert_schedulable(const struct lock_chk_site *site) {
    unused(site);
}

#endif /* DEBUG_LOCK_CHK */

void lock_chk_init(void);
bool lock_chk_tracking_active(void);
