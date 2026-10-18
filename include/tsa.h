/* @title: Thread Safety Analysis */
#pragma once
#include <compiler/core.h>
#include <compiler/name.h>

/* Macros around clang's -Wthread-safety features. Does nothing on GCC
 *
 * Use like this:
 *     struct list_head list TSA_GUARDED_BY(lock);
 *     struct spinlock lock;
 *
 * NOTE: referencing a lock declared later in the struct is given by
 * -fexperimental-late-parse-attributes
 */

#if defined(cn_id_clang)

#define TSA_CAPABILITY(kind) __attribute__((capability(kind)))

/* for regular data */
#define TSA_GUARDED_BY(...) __attribute__((guarded_by(__VA_ARGS__)))

/* This guards the data a POINTER points at, pointer itself is not guarded */
#define TSA_PTR_GUARDED_BY(...) __attribute__((pt_guarded_by(__VA_ARGS__)))

/* Function verification */
#define TSA_CALLER_HOLDS(...) __attribute__((requires_capability(__VA_ARGS__)))
#define TSA_CALLER_HOLDS_SHARED(...)                                           \
    __attribute__((requires_shared_capability(__VA_ARGS__)))
#define TSA_EXCLUDED(...) __attribute__((locks_excluded(__VA_ARGS__)))

/* Lock primitives and wrappers */
#define TSA_ACQUIRES(...) __attribute__((acquire_capability(__VA_ARGS__)))
#define TSA_ACQUIRES_SHARED(...)                                               \
    __attribute__((acquire_shared_capability(__VA_ARGS__)))
#define TSA_RELEASES(...) __attribute__((release_capability(__VA_ARGS__)))
#define TSA_RELEASES_SHARED(...)                                               \
    __attribute__((release_shared_capability(__VA_ARGS__)))

/* Releases whatever mode is held (rwlock unlock) */
#define TSA_RELEASES_GENERIC(...)                                              \
    __attribute__((release_generic_capability(__VA_ARGS__)))
#define TSA_TRY_ACQUIRES(ret, ...)                                             \
    __attribute__((try_acquire_capability(ret, __VA_ARGS__)))
#define TSA_TRY_ACQUIRES_SHARED(ret, ...)                                      \
    __attribute__((try_acquire_shared_capability(ret, __VA_ARGS__)))
#define TSA_ASSERT_CAPABILITY(...)                                             \
    __attribute__((assert_capability(__VA_ARGS__)))
#define TSA_ASSERT_SHARED_CAPABILITY(...)                                      \
    __attribute__((assert_shared_capability(__VA_ARGS__)))

/* function returns (a pointer to) this capability, for guards */
#define TSA_RETURNS_CAPABILITY(cap) __attribute__((lock_returned(cap)))

/* static ordering between sibling members or globals */
#define TSA_ACQUIRED_AFTER(...) __attribute__((acquired_after(__VA_ARGS__)))
#define TSA_ACQUIRED_BEFORE(...) __attribute__((acquired_before(__VA_ARGS__)))

/* escape hatch */
#define TSA_NO_ANALYSIS __attribute__((no_thread_safety_analysis))

/* For intentionally racy accesses and ganders */
#define TSA_UNSAFE(expr)                                                       \
    ({                                                                         \
        _Pragma("clang diagnostic push");                                      \
        _Pragma("clang diagnostic ignored \"-Wthread-safety-analysis\"");      \
        _Pragma("clang diagnostic ignored \"-Wthread-safety-pointer\"");       \
        __auto_type __tsa_unsafe_v = (expr);                                   \
        _Pragma("clang diagnostic pop");                                       \
        __tsa_unsafe_v;                                                        \
    })

/* tsa_unpublished(&o->lock) states that o is not reachable by anyone else yet,
 * so we simply treat the lock as held for the rest of the function */
#define TSA_DEFINE_ASSUME(type)                                                \
    static inline cc_overloadable cc_always_inline cc_unused void              \
    tsa_unpublished_internal(type *cap) TSA_ASSERT_CAPABILITY(cap) {           \
        cc_ignore(cap);                                                        \
    }
#define tsa_unpublished(cap) tsa_unpublished_internal(cap)

#define TSA_CAPABILITY_DECLARE(name, symbol)                                   \
    struct TSA_CAPABILITY(name) __##symbol##_ctx {};                           \
    extern struct __##symbol##_ctx symbol

/* IRQL tracking. Reentrant capabilities give us nested raises and spinlocks,
 * and we exclude IRQL_RAISED for sleeping primitives like mutexes */
#if cn_has_attribute(reentrant_capability)
#define TSA_IRQL_TRACKS_SPINLOCKS 1
#define TSA_REENTRANT_CAPABILITY_DECLARE(name, symbol)                         \
    struct TSA_CAPABILITY(name)                                                \
        __attribute__((reentrant_capability)) __##symbol##_ctx {};             \
    extern struct __##symbol##_ctx symbol
#else
#define TSA_IRQL_TRACKS_SPINLOCKS 0
#define TSA_REENTRANT_CAPABILITY_DECLARE(name, symbol)                         \
    TSA_CAPABILITY_DECLARE(name, symbol)
#endif

#if TSA_IRQL_TRACKS_SPINLOCKS
#define TSA_ACQUIRES_SPIN(lock)                                                \
    __attribute__((acquire_capability(lock, IRQL_RAISED)))
#define TSA_RELEASES_SPIN(lock)                                                \
    __attribute__((release_capability(lock, IRQL_RAISED)))
#define TSA_TRY_ACQUIRES_SPIN(ret, lock)                                       \
    __attribute__((try_acquire_capability(ret, lock, IRQL_RAISED)))
#define TSA_ACQUIRES_SPIN_IRQL TSA_ACQUIRES(IRQL_RAISED)
#define TSA_RELEASES_SPIN_IRQL TSA_RELEASES(IRQL_RAISED)
#else
#define TSA_ACQUIRES_SPIN(lock) TSA_ACQUIRES(lock)
#define TSA_RELEASES_SPIN(lock) TSA_RELEASES(lock)
#define TSA_TRY_ACQUIRES_SPIN(ret, lock) TSA_TRY_ACQUIRES(ret, lock)
#define TSA_ACQUIRES_SPIN_IRQL
#define TSA_RELEASES_SPIN_IRQL
#endif

/* Interrupt primitives, just like IRQL raises, no
 * irq sleeping with MAY_SLEEP */
#if TSA_IRQL_TRACKS_SPINLOCKS
#define TSA_ACQUIRES_IRQS TSA_ACQUIRES(IRQS_DISABLED)
#define TSA_RELEASES_IRQS TSA_RELEASES(IRQS_DISABLED)
#define TSA_ACQUIRES_IRQS_LOCK(lock)                                           \
    __attribute__((acquire_capability(lock, IRQS_DISABLED)))
#define TSA_RELEASES_IRQS_LOCK(lock)                                           \
    __attribute__((release_capability(lock, IRQS_DISABLED)))
#define TSA_MAY_SLEEP                                                          \
    __attribute__((locks_excluded(IRQL_RAISED, IRQS_DISABLED)))
#else
#define TSA_ACQUIRES_IRQS
#define TSA_RELEASES_IRQS
#define TSA_ACQUIRES_IRQS_LOCK(lock) TSA_ACQUIRES(lock)
#define TSA_RELEASES_IRQS_LOCK(lock) TSA_RELEASES(lock)
#define TSA_MAY_SLEEP TSA_EXCLUDED(IRQL_RAISED)
#endif

#else

#define TSA_CAPABILITY(kind)
#define TSA_GUARDED_BY(...)
#define TSA_PT_GUARDED_BY(...)
#define TSA_CALLER_HOLDS(...)
#define TSA_CALLER_HOLDS_SHARED(...)
#define TSA_EXCLUDED(...)
#define TSA_ACQUIRES(...)
#define TSA_ACQUIRES_SHARED(...)
#define TSA_RELEASES(...)
#define TSA_RELEASES_SHARED(...)
#define TSA_RELEASES_GENERIC(...)
#define TSA_TRY_ACQUIRES(ret, ...)
#define TSA_TRY_ACQUIRES_SHARED(ret, ...)
#define TSA_ASSERT_CAPABILITY(...)
#define TSA_ASSERT_SHARED_CAPABILITY(...)
#define TSA_RETURNS_CAPABILITY(cap)
#define TSA_ACQUIRED_AFTER(...)
#define TSA_ACQUIRED_BEFORE(...)
#define TSA_NO_ANALYSIS
#define TSA_UNSAFE(expr) (expr)
#define TSA_DEFINE_ASSUME(type)
#define tsa_unpublished(cap) ((void) (cap))
#define TSA_CAPABILITY_DECLARE(name, symbol)
#define TSA_IRQL_TRACKS_SPINLOCKS 0
#define TSA_REENTRANT_CAPABILITY_DECLARE(name, symbol)
#define TSA_ACQUIRES_SPIN(lock)
#define TSA_RELEASES_SPIN(lock)
#define TSA_TRY_ACQUIRES_SPIN(ret, lock)
#define TSA_ACQUIRES_SPIN_IRQL
#define TSA_RELEASES_SPIN_IRQL
#define TSA_ACQUIRES_IRQS
#define TSA_RELEASES_IRQS
#define TSA_ACQUIRES_IRQS_LOCK(lock)
#define TSA_RELEASES_IRQS_LOCK(lock)
#define TSA_MAY_SLEEP

#endif

/* Token for fields that only a certain thread (the owner) should touch,
 * like
 *
 *     TSA_OWNER_TOKEN(self);
 *     uint32_t rcu_nesting TSA_GUARDED_BY(self);
 *
 * Code running under the owner says tsa_owner_is_me(&t->self) */
struct TSA_CAPABILITY("owner") tsa_owner {};
#define TSA_OWNER_TOKEN(name) struct tsa_owner name

static inline cc_always_inline
cc_unused void tsa_owner_is_me(struct tsa_owner *tok)
    TSA_ASSERT_CAPABILITY(tok) {
    cc_ignore(tok);
}
