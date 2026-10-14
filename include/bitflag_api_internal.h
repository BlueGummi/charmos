#include <compiler/core.h>
#include <math/bit.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define BR_BITFLAG_RANGE_ASSERT(lo, hi)                                        \
    (sizeof(struct {                                                           \
         static_assert((hi) >= (lo), "hi must be >= lo");                      \
         uint8_t _b;                                                           \
     }) *                                                                      \
     0)

#define BR_EVAL(...) BR_EVAL64(__VA_ARGS__)
#define BR_EVAL64(...) BR_EVAL32(BR_EVAL32(__VA_ARGS__))
#define BR_EVAL32(...) BR_EVAL16(BR_EVAL16(__VA_ARGS__))
#define BR_EVAL16(...) BR_EVAL8(BR_EVAL8(__VA_ARGS__))
#define BR_EVAL8(...) BR_EVAL4(BR_EVAL4(__VA_ARGS__))
#define BR_EVAL4(...) BR_EVAL2(BR_EVAL2(__VA_ARGS__))
#define BR_EVAL2(...) BR_EVAL1(BR_EVAL1(__VA_ARGS__))
#define BR_EVAL1(...) __VA_ARGS__

#define BR_EMPTY()
#define BR_DEFER(id) id BR_EMPTY()

#define BR_IIF(cond) PP_CONCAT(BR_IIF_, cond)
#define BR_IIF_0(t, f) f
#define BR_IIF_1(t, f) t

#define BR_HAS_COMMA(...) BR_HAS_COMMA_EVAL(__VA_ARGS__, 1, 0, 0)
#define BR_HAS_COMMA_EVAL(_1, _2, N, ...) N
#define BR_CMP_TOKEN(a, b) PP_CONCAT(BR_CMP_, PP_CONCAT(a, PP_CONCAT(_, b)))
#define BR_IS_EQUAL(a, b) BR_HAS_COMMA(BR_CMP_TOKEN(a, b))

#define BR_CMP_0_0 ,
#define BR_CMP_1_1 ,
#define BR_CMP_2_2 ,
#define BR_CMP_3_3 ,
#define BR_CMP_4_4 ,
#define BR_CMP_5_5 ,
#define BR_CMP_6_6 ,
#define BR_CMP_7_7 ,
#define BR_CMP_8_8 ,
#define BR_CMP_9_9 ,
#define BR_CMP_10_10 ,
#define BR_CMP_11_11 ,
#define BR_CMP_12_12 ,
#define BR_CMP_13_13 ,
#define BR_CMP_14_14 ,
#define BR_CMP_15_15 ,
#define BR_CMP_16_16 ,
#define BR_CMP_17_17 ,
#define BR_CMP_18_18 ,
#define BR_CMP_19_19 ,
#define BR_CMP_20_20 ,
#define BR_CMP_21_21 ,
#define BR_CMP_22_22 ,
#define BR_CMP_23_23 ,
#define BR_CMP_24_24 ,
#define BR_CMP_25_25 ,
#define BR_CMP_26_26 ,
#define BR_CMP_27_27 ,
#define BR_CMP_28_28 ,
#define BR_CMP_29_29 ,
#define BR_CMP_30_30 ,
#define BR_CMP_31_31 ,

#define BR_INC(x) PP_CONCAT(BR_INC_, x)
#define BR_INC_0 1
#define BR_INC_1 2
#define BR_INC_2 3
#define BR_INC_3 4
#define BR_INC_4 5
#define BR_INC_5 6
#define BR_INC_6 7
#define BR_INC_7 8
#define BR_INC_8 9
#define BR_INC_9 10
#define BR_INC_10 11
#define BR_INC_11 12
#define BR_INC_12 13
#define BR_INC_13 14
#define BR_INC_14 15
#define BR_INC_15 16
#define BR_INC_16 17
#define BR_INC_17 18
#define BR_INC_18 19
#define BR_INC_19 20
#define BR_INC_20 21
#define BR_INC_21 22
#define BR_INC_22 23
#define BR_INC_23 24
#define BR_INC_24 25
#define BR_INC_25 26
#define BR_INC_26 27
#define BR_INC_27 28
#define BR_INC_28 29
#define BR_INC_29 30
#define BR_INC_30 31

#define BR_RANGE_STOP(curr, hi)
#define BR_RANGE_CONT(curr, hi)                                                \
    BR_DEFER(BR_RANGE_STEP_INDIRECT)()(BR_INC(curr), hi)
#define BR_RANGE_STEP_INDIRECT() BR_RANGE_STEP

#define BR_RANGE_STEP(curr, hi)                                                \
    , curr BR_IIF(BR_IS_EQUAL(curr, hi))(BR_RANGE_STOP, BR_RANGE_CONT)(curr, hi)

#define BR_RANGE_GENERATE_I(lo, hi)                                            \
    (BR_BITFLAG_RANGE_ASSERT(lo, hi) + (lo)) BR_IIF(BR_IS_EQUAL(lo, hi))(      \
        BR_RANGE_STOP, BR_RANGE_STEP)(BR_INC(lo), hi)

#define BITFLAG_RANGE(lo, hi) BR_EVAL(BR_RANGE_GENERATE_I(lo, hi))

// clang-format off

#define BR_MASK_BIT(type, idx) (((type) 1) << (idx))

#define BR_MASK_1(type, x) BR_MASK_BIT(type, x)
#define BR_MASK_2(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_1(type, __VA_ARGS__))
#define BR_MASK_3(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_2(type, __VA_ARGS__))
#define BR_MASK_4(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_3(type, __VA_ARGS__))
#define BR_MASK_5(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_4(type, __VA_ARGS__))
#define BR_MASK_6(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_5(type, __VA_ARGS__))
#define BR_MASK_7(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_6(type, __VA_ARGS__))
#define BR_MASK_8(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_7(type, __VA_ARGS__))
#define BR_MASK_9(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_8(type, __VA_ARGS__))
#define BR_MASK_10(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_9(type, __VA_ARGS__))
#define BR_MASK_11(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_10(type, __VA_ARGS__))
#define BR_MASK_12(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_11(type, __VA_ARGS__))
#define BR_MASK_13(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_12(type, __VA_ARGS__))
#define BR_MASK_14(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_13(type, __VA_ARGS__))
#define BR_MASK_15(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_14(type, __VA_ARGS__))
#define BR_MASK_16(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_15(type, __VA_ARGS__))
#define BR_MASK_17(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_16(type, __VA_ARGS__))
#define BR_MASK_18(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_17(type, __VA_ARGS__))
#define BR_MASK_19(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_18(type, __VA_ARGS__))
#define BR_MASK_20(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_19(type, __VA_ARGS__))
#define BR_MASK_21(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_20(type, __VA_ARGS__))
#define BR_MASK_22(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_21(type, __VA_ARGS__))
#define BR_MASK_23(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_22(type, __VA_ARGS__))
#define BR_MASK_24(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_23(type, __VA_ARGS__))
#define BR_MASK_25(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_24(type, __VA_ARGS__))
#define BR_MASK_26(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_25(type, __VA_ARGS__))
#define BR_MASK_27(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_26(type, __VA_ARGS__))
#define BR_MASK_28(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_27(type, __VA_ARGS__))
#define BR_MASK_29(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_28(type, __VA_ARGS__))
#define BR_MASK_30(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_29(type, __VA_ARGS__))
#define BR_MASK_31(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_30(type, __VA_ARGS__))
#define BR_MASK_32(type, x, ...) (BR_MASK_BIT(type, x) | BR_MASK_31(type, __VA_ARGS__))

// clang-format on

#define BR_MAKE_MASK_IMPL(type, ...)                                           \
    PP_OVERLOAD(BR_MASK, __VA_ARGS__)(type, __VA_ARGS__)

#define BR_MAKE_MASK(type, ...) BR_MAKE_MASK_IMPL(type, __VA_ARGS__)
