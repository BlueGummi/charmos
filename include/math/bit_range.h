/* @title: Bit Ranges */
#define BIT_RANGE(val, lo, hi)                                                 \
    (((val) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

#define BIT_MASK(lo, hi) (((1u << ((hi) - (lo) + 1)) - 1) << (lo))

#define SET_FIELD(val, field_val, lo, hi)                                      \
    (((val) & ~BIT_MASK(lo, hi)) | (((field_val) << (lo)) & BIT_MASK(lo, hi)))
