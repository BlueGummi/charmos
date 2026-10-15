/* @title: Sequence Lock */
#pragma once
#include <atomic.h>
#include <compiler/atomic.h>
#include <compiler/core.h>
#include <enforce.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spinlock.h>

/* The naming here is rather... unpleasant, so it's worth specifying upfront:
 *
 * seqcount_ is for all the struct seqcount functions, seq_ and seqlock_
 * are for the struct seqlock functions, we keep it this way so that
 * function signatures don't explode in length, and to mirror
 * the general naming conventions of the lock primitives with lock
 * in their name (see spinlock.h, rwlock.h)
 *
 * (begin_|end_)(read|write)(_raw)(_high)(_retry)
 *
 * is the syntax ordering, broadly */

/*
 * Sequence counters for lock free reader
 * synchronization with serialized writers
 *
 * Odd sequence indicates an in progress write,
 * even count indicates quiescent data.
 *
 * TODO: the thread API and thread.c uses what is effectively
 * a sequence counter, just not with this API. someday we can change it over
 */
struct seqcount {
    atomic_uint32_t sequence;
};
typedef struct seqcount seqcount_t;

#define SEQCOUNT_INIT                                                          \
    (struct seqcount) {                                                        \
        .sequence = ATOMIC_VAR_INIT(0)                                         \
    }

static inline void seqcount_init(struct seqcount *s) {
    atomic_store_rlx(&s->sequence, 0);
}

static inline uint32_t seqcount_read_raw(const struct seqcount *s) {
    return atomic_load_rlx(&s->sequence);
}

static inline uint32_t seqcount_begin_read_raw(const struct seqcount *s) {
    uint32_t ret = seqcount_read_raw(s);
    ca_rmb();
    return ret;
}

/*
 * Wait for any active writer to complete and return
 * the sequence with acquire barrier
 */
static inline uint32_t seqcount_begin_read(const struct seqcount *s) {
    while (true) {
        uint32_t seq = seqcount_read_raw(s);
        if (cc_likely((seq & 1) == 0)) {
            ca_rmb();
            return seq;
        }
        cpu_pause();
    }
}

/* Check for change since `start` */
static inline bool seqcount_read_retry(const struct seqcount *s,
                                       uint32_t start) {
    ca_rmb();
    return cc_unlikely(seqcount_read_raw(s) != start);
}

/*
 * NOTE: the separate rlx load and store here is intentional:
 * atomic_fetch_add is RMW, and unneeded when there is only one writer
 */

/* even to odd with wmb */
static inline void seqcount_begin_write(struct seqcount *s) {
    uint32_t seq = seqcount_read_raw(s);
    atomic_store_rlx(&s->sequence, seq + 1);
    ca_wmb();
}

/* odd to even with wmb */
static inline void seqcount_end_write(struct seqcount *s) {
    ca_wmb();
    uint32_t seq = seqcount_read_raw(s);
    atomic_store_rlx(&s->sequence, seq + 1);
}

/*
 * Sequence Lock (seqlock)
 *
 * Combines a sequence counter with a spinlock to serialize writers
 */
struct seqlock {
    struct seqcount seqcount;
    struct spinlock lock;
};
typedef struct seqlock seqlock_t;

static inline void seqlock_init_chk_full(struct seqlock *sl,
                                         const struct lock_chk_class *class,
                                         enum lock_chk_flags flags) {
    cc_ignore(class, flags);
    seqcount_init(&sl->seqcount);
    spinlock_init_chk(&sl->lock, class, flags);
}

#define SEQLOCK_INIT_CHK(class_, flags_)                                       \
    (struct seqlock) {                                                         \
        .seqcount = SEQCOUNT_INIT,                                             \
        .lock = SPINLOCK_INIT_CHK((class_), (flags_))                          \
    }

#define SEQLOCK_INIT SEQLOCK_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define SEQLOCK_DEFINE(id) struct seqlock id = SEQLOCK_INIT
#define SEQLOCK_DEFINE_CHK(id, class_, flags_)                                 \
    struct seqlock id = SEQLOCK_INIT_CHK((class_), (flags_))

#ifdef DEBUG_LOCK_CHK

#define seqlock_init_chk(sl_, class_, flags_)                                  \
    seqlock_init_chk_full((sl_), (class_), (flags_))
#define seqlock_init_auto_internal(sl_, flags_)                                \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #sl_,                                                      \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        seqlock_init_chk_full((sl_), &__auto_class, (flags_));                 \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define seqlock_init_chk(sl_, class_, flags_)                                  \
    seqlock_init_chk_full((sl_), NULL, LOCK_UNCHKD)
#define seqlock_init_auto_internal(sl_, flags_)                                \
    seqlock_init_chk_full((sl_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define seqlock_init_1(sl_) seqlock_init_2(sl_, LOCK_CHKD_FULL)
#define seqlock_init_2(sl_, flags_) seqlock_init_auto_internal((sl_), (flags_))
#define seqlock_init(...) PP_CALL(seqlock_init, __VA_ARGS__)

static inline uint32_t seq_begin_read(const struct seqlock *sl) {
    return seqcount_begin_read(&sl->seqcount);
}

static inline bool seq_read_retry(const struct seqlock *sl, uint32_t start) {
    return seqcount_read_retry(&sl->seqcount, start);
}

static inline uint32_t seq_begin_read_raw(const struct seqlock *sl) {
    return seqcount_begin_read_raw(&sl->seqcount);
}

static inline uint32_t seq_read_raw(const struct seqlock *sl) {
    return seqcount_read_raw(&sl->seqcount);
}

static inline cc_warn_unused_result enum irql seq_write_lock(struct seqlock *sl)
    TSA_ACQUIRES_SPIN(&sl->lock) {
    enum irql irql = spin_lock(&sl->lock);
    seqcount_begin_write(&sl->seqcount);
    return irql;
}

/* Writer APIs */
static inline cc_warn_unused_result enum irql
seq_write_lock_high(struct seqlock *sl) TSA_ACQUIRES_SPIN(&sl->lock) {
    enum irql irql = spin_lock_high(&sl->lock);
    seqcount_begin_write(&sl->seqcount);
    return irql;
}

static inline void seq_write_unlock(struct seqlock *sl, enum irql old)
    TSA_RELEASES_SPIN(&sl->lock) {
    seqcount_end_write(&sl->seqcount);
    spin_unlock(&sl->lock, old);
}

/* Raw, no IRQL */
static inline void seq_write_lock_raw(struct seqlock *sl)
    TSA_ACQUIRES(&sl->lock) {
    spin_lock_raw(&sl->lock);
    seqcount_begin_write(&sl->seqcount);
}

static inline void seq_write_unlock_raw(struct seqlock *sl)
    TSA_RELEASES(&sl->lock) {
    seqcount_end_write(&sl->seqcount);
    spin_unlock_raw(&sl->lock);
}

/* Trylock */
static inline cc_warn_unused_result bool seq_try_write_lock(struct seqlock *sl,
                                                            enum irql *out)
    TSA_TRY_ACQUIRES(true, &sl->lock) {
    if (spin_trylock(&sl->lock, out)) {
        seqcount_begin_write(&sl->seqcount);
        return true;
    }
    return false;
}

static inline cc_warn_unused_result bool
seq_try_write_lock_high(struct seqlock *sl, enum irql *out)
    TSA_TRY_ACQUIRES(true, &sl->lock) {
    if (spin_trylock_high(&sl->lock, out)) {
        seqcount_begin_write(&sl->seqcount);
        return true;
    }
    return false;
}

static inline cc_warn_unused_result bool
seq_try_write_lock_raw(struct seqlock *sl) TSA_TRY_ACQUIRES(true, &sl->lock) {
    if (spin_trylock_raw(&sl->lock)) {
        seqcount_begin_write(&sl->seqcount);
        return true;
    }
    return false;
}

/* Query */
static inline bool seqlock_is_writing(const struct seqlock *sl) {
    return (seqcount_read_raw(&sl->seqcount) & 1) != 0;
}

static inline bool seqlock_held(const struct seqlock *sl) {
    return spinlock_locked((struct spinlock *) &sl->lock);
}

#define SEQLOCK_ASSERT_HELD(sl) enforce(seqlock_held(sl))

static inline cc_always_inline
cc_unused void seqcount_write_guard_exit(struct seqcount **s) {
    if (*s)
        seqcount_end_write(*s);
}

static inline cc_always_inline cc_unused struct seqcount *
seqcount_write_guard_enter(struct seqcount *s) {
    seqcount_begin_write(s);
    return s;
}

struct seqlock_write_guard {
    struct seqlock *sl;
    enum irql irql;
};

struct seqlock_write_raw_guard {
    struct seqlock *sl;
};

static inline cc_always_inline cc_unused void
seqlock_write_guard_assume(struct seqlock *sl)
    TSA_ASSERT_CAPABILITY(&sl->lock) {
    cc_ignore(sl);
}

static inline cc_always_inline cc_unused void seqlock_write_guard_exit(
    struct seqlock_write_guard *g) TSA_RELEASES_SPIN_IRQL TSA_NO_ANALYSIS {
    if (g->sl)
        seq_write_unlock(g->sl, g->irql);
}

static inline cc_always_inline cc_unused struct seqlock_write_guard
seqlock_write_guard_enter(struct seqlock *sl)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct seqlock_write_guard) {
        .sl = sl,
        .irql = seq_write_lock(sl),
    };
}

static inline cc_always_inline cc_unused struct seqlock_write_guard
seqlock_write_high_guard_enter(struct seqlock *sl)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct seqlock_write_guard) {
        .sl = sl,
        .irql = seq_write_lock_high(sl),
    };
}

static inline cc_always_inline cc_unused void seqlock_write_raw_guard_exit(
    struct seqlock_write_raw_guard *g) TSA_NO_ANALYSIS {
    if (g->sl)
        seq_write_unlock_raw(g->sl);
}

static inline cc_always_inline cc_unused struct seqlock_write_raw_guard
seqlock_write_raw_guard_enter(struct seqlock *sl) TSA_NO_ANALYSIS {
    seq_write_lock_raw(sl);
    return (struct seqlock_write_raw_guard) {.sl = sl};
}

#define seqcount_write_guard(s_)                                               \
    cc_cleanup(seqcount_write_guard_exit) struct seqcount *PP_CONCAT(          \
        seqc_guard_, __COUNTER__) = seqcount_write_guard_enter(s_)

#define seq_write_guard(sl_)                                                   \
    cc_cleanup(seqlock_write_guard_exit) struct seqlock_write_guard PP_CONCAT( \
        seq_wguard_, __COUNTER__) =                                            \
        (seqlock_write_guard_assume(sl_), seqlock_write_guard_enter(sl_))

#define seq_write_guard_high(sl_)                                              \
    cc_cleanup(seqlock_write_guard_exit) struct seqlock_write_guard PP_CONCAT( \
        seq_wguard_high_, __COUNTER__) =                                       \
        (seqlock_write_guard_assume(sl_), seqlock_write_high_guard_enter(sl_))

#define seq_write_guard_raw(sl_)                                               \
    cc_cleanup(seqlock_write_raw_guard_exit) struct seqlock_write_raw_guard    \
    PP_CONCAT(seq_wguard_raw_, __COUNTER__) =                                  \
        (seqlock_write_guard_assume(sl_), seqlock_write_raw_guard_enter(sl_))
