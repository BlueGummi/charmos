/* @title: Error Codes */
#pragma once
#include <compiler.h>
#include <console/panic.h>
#include <kassert.h>
#include <linker/symbols.h>
#include <stdarg.h>
#include <stdint.h>

#define ERR_IS_FATAL(e) (e != ERR_OK && e != ERR_AGAIN)

#define ERR_IS_MATCH_1(v, a) ((v) == (a))
#define ERR_IS_MATCH_2(v, a, b) (ERR_IS_MATCH_1(v, a) || ((v) == (b)))
#define ERR_IS_MATCH_3(v, a, b, c) (ERR_IS_MATCH_2(v, a, b) || ((v) == (c)))
#define ERR_IS_MATCH_4(v, a, b, c, d)                                          \
    (ERR_IS_MATCH_3(v, a, b, c) || ((v) == (d)))
#define ERR_IS_MATCH_5(v, a, b, c, d, e)                                       \
    (ERR_IS_MATCH_4(v, a, b, c, d) || ((v) == (e)))
#define ERR_IS_MATCH_6(v, a, b, c, d, e, f)                                    \
    (ERR_IS_MATCH_5(v, a, b, c, d, e) || ((v) == (f)))
#define ERR_IS_MATCH_7(v, a, b, c, d, e, f, g)                                 \
    (ERR_IS_MATCH_6(v, a, b, c, d, e, f) || ((v) == (g)))
#define ERR_IS_MATCH_8(v, a, b, c, d, e, f, g, h)                              \
    (ERR_IS_MATCH_7(v, a, b, c, d, e, f, g) || ((v) == (h)))

#define ERR_GUARD(val, ...)                                                    \
    ({                                                                         \
        __auto_type _v = (val);                                                \
        if (unlikely(_v < 0 && !_DISPATCH(ERR_IS_MATCH, PP_NARG(__VA_ARGS__))( \
                                   _v, __VA_ARGS__)))                          \
            panic("unhandled error: %s", errno_to_str(_v));                    \
        _v;                                                                    \
    })

/* When this enum errno is negated, i.e. becomes positive, the upper 16 bits
 * indicate the *facility*. With 0, it's just one of these, but if it's > 0,
 * it came from a specific subsystem that defined a struct err_facility */
enum errno {
    ERR_OK = 0,          // Success
    ERR_UNKNOWN = -1,    // Unknown or unspecified error
    ERR_NO_MEM = -2,     // Out of memory
    ERR_NO_DEV = -3,     // No such device
    ERR_NO_ENT = -4,     // No such file or directory
    ERR_EXIST = -5,      // File already exists
    ERR_IO = -6,         // I/O error
    ERR_NOT_DIR = -7,    // Not a directory
    ERR_IS_DIR = -8,     // Is a directory
    ERR_INVAL = -9,      // Invalid argument
    ERR_PERM = -10,      // Permission denied
    ERR_FAULT = -11,     // Bad address
    ERR_BUSY = -12,      // Resource/device busy
    ERR_AGAIN = -13,     // Try again later
    ERR_NOT_IMPL = -14,  // Not implemented
    ERR_NOSPC = -15,     // No space left on device
    ERR_OVERFLOW = -16,  // Value too large
    ERR_NOT_EMPTY = -17, // Directory not empty

};

struct err_facility {
    uint16_t prefix;
    const char *name;
    const char *desc;
    const char *(*const to_str)(uint16_t delta);
};

/* Deltas must always be positive */
#define ERR_CREATE(pre, del)                                                   \
    ({                                                                         \
        kassert((del & 0xFFFF) == del);                                        \
        kassert(del > 0);                                                      \
        -((((int) (pre)) << 16) | (((int) (del)) & 0xFFFF));                   \
    })

#define ERR_GET_FACILITY(e)                                                    \
    ({                                                                         \
        kassert(e < 0);                                                        \
        ((-(e)) >> 16) & 0xFFFF;                                               \
    })

#define ERR_GET_DELTA(e)                                                       \
    ({                                                                         \
        kassert(e < 0);                                                        \
        ((-(e)) & 0xFFFF);                                                     \
    })

#define ERR_PREFIX(n) ((__err_facility_##n).prefix)
#define ERR_DELTA_START (1)
#define ERR(n, d) ERR_CREATE(ERR_PREFIX(n), d)

#define ERR_FACILITY(n) __err_facility_##n
#define ERR_FACILITY_EXTERN(n) extern struct err_facility __err_facility_##n
#define ERR_FACILITY_DECLARE(n, ...)                                           \
    LINKER_SECTION_OBJECT(struct err_facility, err_facilities)                 \
    __err_facility_##n = {.name = #n, __VA_ARGS__}

LINKER_SECTION_DEFINE(struct err_facility, err_facilities);

const char *errno_facility_to_str(enum errno err);
void err_facilities_init();

static inline const char *errno_to_str(enum errno err) {
    switch (err) {
    case ERR_OK: return "No error";
    case ERR_UNKNOWN: return "Unknown error";
    case ERR_NO_MEM: return "Out of memory";
    case ERR_NO_DEV: return "No such device";
    case ERR_NO_ENT: return "No such file or directory";
    case ERR_EXIST: return "File already exists";
    case ERR_IO: return "I/O error";
    case ERR_NOT_DIR: return "Not a directory";
    case ERR_IS_DIR: return "Is a directory";
    case ERR_INVAL: return "Invalid argument";
    case ERR_PERM: return "Permission denied";
    case ERR_FAULT: return "Bad memory access";
    case ERR_BUSY: return "Resource busy";
    case ERR_AGAIN: return "Try again";
    case ERR_NOT_IMPL: return "Not implemented";
    case ERR_NOSPC: return "No space left";
    case ERR_OVERFLOW: return "Value too large";
    case ERR_NOT_EMPTY: return "Directory not empty";

    default: return errno_facility_to_str(err);
    }
}
