/* @title: One-time atomic tokens */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <stdbool.h>

#define ONCE_TOKEN_DEFINE(type, name)                                          \
    struct {                                                                   \
        atomic(type) state;                                                    \
    } name

#define ONCE_TOKEN_INIT(val) {.state = ATOMIC_VAR_INIT(val)}

/* ===== typecheck ===== */
#define once_token_typecheck_internal(tok) ((void) sizeof((tok)->state))

#define once_token_typecheck_internal_type(tok, type)                          \
    ct_typecheck(type, atomic_load_rlx(&(tok)->state))

#define once_token_typecheck_internal_bool(tok)                                \
    once_token_typecheck_internal_type(tok, bool)

#define once_token_typecheck_internal_same(tok1, tok2)                         \
    ct_typecheck_same(atomic_load_rlx(&(tok1)->state),                         \
                      atomic_load_rlx(&(tok2)->state))

/* ===== init ===== */
#define once_token_init_1(tok)                                                 \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal_bool(__ot_tok);                          \
        static_assert(cc_generic((__ot_tok->state), bool : 1, default : 0),    \
                      "once_token_init_1 requires a boolean token");           \
        atomic_init(&__ot_tok->state, false);                                  \
    })

#define once_token_init_2(tok, val)                                            \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal(__ot_tok);                               \
        typeof(atomic_load_rlx(&__ot_tok->state)) __ot_val = (val);            \
        atomic_init(&__ot_tok->state, __ot_val);                               \
    })

#define once_token_init(...) PP_CALL(once_token_init, __VA_ARGS__)

/* ===== claim ===== */
/* The 2 and 4 argument versions return what was actually there, which
 * can be used to inspect whether something unexpected happened */
#define once_token_claim_4(tok, from, to, out)                                 \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal(__ot_tok);                               \
        ct_typecheck_same(from, to);                                           \
        typeof(atomic_load_rlx(&__ot_tok->state)) __ot_exp = (from);           \
        typeof(__ot_exp) __ot_to = (to);                                       \
        __auto_type __ot_out = (out);                                          \
        static_assert(ct_is_type(typeof(__ot_exp) *, out) ||                   \
                          (ct_is_const(out) && (out) == NULL),                 \
                      "once_token_claim out must point to the token state "    \
                      "type or be NULL");                                      \
        bool ret = atomic_cas_strong(&__ot_tok->state, &__ot_exp, __ot_to,     \
                                     mo_acq_rel, mo_acquire);                  \
        if (__ot_out)                                                          \
            *((typeof(__ot_exp) *) __ot_out) = __ot_exp;                       \
        ret;                                                                   \
    })

#define once_token_claim_3(tok, from, to)                                      \
    once_token_claim_4(tok, from, to, NULL)

#define once_token_claim_2(tok, out)                                           \
    ({                                                                         \
        once_token_typecheck_internal_bool(tok);                               \
        static_assert(cc_generic(((tok)->state), bool : 1, default : 0),       \
                      "once_token_claim_1 argument requires a boolean token"); \
        once_token_claim_4(tok, false, true, out);                             \
    })

#define once_token_claim_1(tok) once_token_claim_2(tok, NULL)

#define once_token_claim(...) PP_CALL(once_token_claim, __VA_ARGS__)

/* ===== claimed ===== */
#define once_token_claimed_1(tok)                                              \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal_bool(__ot_tok);                          \
        static_assert(cc_generic((__ot_tok->state), bool : 1, default : 0),    \
                      "once_token_claimed_1 requires a boolean token");        \
                                                                               \
        atomic_load_acq(&__ot_tok->state);                                     \
    })

#define once_token_claimed_2(tok, val)                                         \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal(__ot_tok);                               \
        typeof(atomic_load_rlx(&__ot_tok->state)) __ot_val = (val);            \
                                                                               \
        atomic_load_acq(&__ot_tok->state) == __ot_val;                         \
    })

#define once_token_claimed(...) PP_CALL(once_token_claimed, __VA_ARGS__)

/* ===== reset ===== */
#define once_token_reset_1(tok)                                                \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal_bool(__ot_tok);                          \
        static_assert(cc_generic((__ot_tok->state), bool : 1, default : 0),    \
                      "once_token_reset_1 requires a boolean token");          \
        atomic_store_release(&__ot_tok->state, false);                         \
    })

#define once_token_reset_2(tok, val)                                           \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal(__ot_tok);                               \
        typeof(atomic_load_rlx(&__ot_tok->state)) __ot_val = (val);            \
        atomic_store_release(&__ot_tok->state, __ot_val);                      \
    })

#define once_token_reset(...) PP_CALL(once_token_reset, __VA_ARGS__)

#define once_token_read(tok)                                                   \
    ({                                                                         \
        __auto_type __ot_tok = (tok);                                          \
        once_token_typecheck_internal(__ot_tok);                               \
        atomic_load_acq(&__ot_tok->state);                                     \
    })
