/* @title: Lock Validation */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lock_chk_class;
struct lock_chk_lock;

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
    LOCK_CHK_MODE_IGNORED,
    LOCK_CHK_MODE_SHARED,
    LOCK_CHK_MODE_EXCLUSIVE,
};

enum lock_chk_engine_state : uint8_t {
    LOCK_CHK_INACTIVE,
    LOCK_CHK_ACTIVE,
    LOCK_CHK_DEGRADED,
};

#define LOCK_OP_IRQ_MASK 0x3
#define LOCK_OP_KIND_MASK 0x18

enum lock_op_flags : uint8_t {
    LOCK_OP_IRQ_NONE = 0,
    LOCK_OP_IRQ_DISPATCH = 1,
    LOCK_OP_IRQ_HIGH = 2,
    LOCK_OP_RAW = 1 << 2,

    /* These are represented NOT as a single bit
     * so a missing KIND can trip an assertion
     * instead of assuming a state */
    LOCK_OP_KIND_BLOCKING = 1 << 3,
    LOCK_OP_KIND_TRY = 2 << 3,
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

#define LOCK_CHK_CLASS_DEFINE(id)                                              \
    extern const struct lock_chk_class __lock_chk_class_##id;                  \
    const struct lock_chk_class __lock_chk_class_##id = {                      \
        .name = #id,                                                           \
        .file = __RELFILE__,                                                   \
        .line = __LINE__,                                                      \
    }

#define LOCK_CHK_CLASS_DEFINE_LOCAL(id)                                        \
    static const struct lock_chk_class __lock_chk_class_##id = {               \
        .name = #id,                                                           \
        .file = __RELFILE__,                                                   \
        .line = __LINE__,                                                      \
    }

#define LOCK_CHK_CLASS_EXTERN(id)                                              \
    extern const struct lock_chk_class __lock_chk_class_##id

#else /* !defined(DEBUG_LOCK_CHK) */

#define LOCK_CHK_SITE_HERE() ((const struct lock_chk_site *) NULL)
#define LOCK_CHK_CLASS(id) ((const struct lock_chk_class *) NULL)
#define LOCK_CHK_CLASS_DEFINE(id)
#define LOCK_CHK_CLASS_DEFINE_LOCAL(id)
#define LOCK_CHK_CLASS_EXTERN(id)

#endif /* DEBUG_LOCK_CHK */

/*
 * Prototypes
 */
#ifdef DEBUG_LOCK_CHK

void lock_debug_spin_classify(atomic(enum lock_op_flags) * usage,
                              enum lock_op_flags requested,
                              struct lock_chk_lock *lock,
                              const struct lock_chk_site *site);
bool lock_debug_spin_push(struct lock_chk_lock *lock, enum irql prev_irql,
                          const struct lock_chk_site *site);
void lock_debug_spin_validate_top(struct lock_chk_lock *lock,
                                  enum irql prev_irql,
                                  const struct lock_chk_site *site);
void lock_debug_spin_pop(struct lock_chk_lock *lock);

void lock_chk_note_use(struct lock_chk_lock *lock, enum lock_op_flags flags);

void lock_chk_assert_schedulable(const struct lock_chk_site *site);

#else /* !defined(DEBUG_LOCK_CHK) */

static inline void lock_chk_note_lock_use(struct lock_chk_lock *lock,
                                          bool manages_irql,
                                          bool raw_operation) {
    cc_ignore(lock, manages_irql, raw_operation);
}

static inline void
lock_chk_assert_schedulable(const struct lock_chk_site *site) {
    cc_ignore(site);
}

#endif /* DEBUG_LOCK_CHK */

void lock_chk_init(void);
bool lock_chk_tracking_active(void);
