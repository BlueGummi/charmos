/* @title: Min and Max macros */
#pragma once
#include <compiler.h>

#define _MIN_1(a) (a)

#define _MIN_2(a, b)                                                           \
    ({                                                                         \
        __auto_type _a = (a);                                                  \
        __auto_type _b = (b);                                                  \
        _a < _b ? _a : _b;                                                     \
    })

#define _MIN_3(a, b, c)                                                        \
    ({                                                                         \
        __auto_type _m = _MIN_2(a, b);                                         \
        __auto_type _c = (c);                                                  \
        _m < _c ? _m : _c;                                                     \
    })

#define _MIN_4(a, b, c, d)                                                     \
    ({                                                                         \
        __auto_type _m = _MIN_3(a, b, c);                                      \
        __auto_type _d = (d);                                                  \
        _m < _d ? _m : _d;                                                     \
    })

#define _MIN_5(a, b, c, d, e)                                                  \
    ({                                                                         \
        __auto_type _m = _MIN_4(a, b, c, d);                                   \
        __auto_type _e = (e);                                                  \
        _m < _e ? _m : _e;                                                     \
    })

#define _MIN_6(a, b, c, d, e, f)                                               \
    ({                                                                         \
        __auto_type _m = _MIN_5(a, b, c, d, e);                                \
        __auto_type _f = (f);                                                  \
        _m < _f ? _m : _f;                                                     \
    })

#define _MIN_7(a, b, c, d, e, f, g)                                            \
    ({                                                                         \
        __auto_type _m = _MIN_6(a, b, c, d, e, f);                             \
        __auto_type _g = (g);                                                  \
        _m < _g ? _m : _g;                                                     \
    })

#define _MIN_8(a, b, c, d, e, f, g, h)                                         \
    ({                                                                         \
        __auto_type _m = _MIN_7(a, b, c, d, e, f, g);                          \
        __auto_type _h = (h);                                                  \
        _m < _h ? _m : _h;                                                     \
    })

#define MIN(...) _DISPATCH(_MIN, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define _MAX_1(a) (a)

#define _MAX_2(a, b)                                                           \
    ({                                                                         \
        __auto_type _a = (a);                                                  \
        __auto_type _b = (b);                                                  \
        _a > _b ? _a : _b;                                                     \
    })

#define _MAX_3(a, b, c)                                                        \
    ({                                                                         \
        __auto_type _m = _MAX_2(a, b);                                         \
        __auto_type _c = (c);                                                  \
        _m > _c ? _m : _c;                                                     \
    })

#define _MAX_4(a, b, c, d)                                                     \
    ({                                                                         \
        __auto_type _m = _MAX_3(a, b, c);                                      \
        __auto_type _d = (d);                                                  \
        _m > _d ? _m : _d;                                                     \
    })

#define _MAX_5(a, b, c, d, e)                                                  \
    ({                                                                         \
        __auto_type _m = _MAX_4(a, b, c, d);                                   \
        __auto_type _e = (e);                                                  \
        _m > _e ? _m : _e;                                                     \
    })

#define _MAX_6(a, b, c, d, e, f)                                               \
    ({                                                                         \
        __auto_type _m = _MAX_5(a, b, c, d, e);                                \
        __auto_type _f = (f);                                                  \
        _m > _f ? _m : _f;                                                     \
    })

#define _MAX_7(a, b, c, d, e, f, g)                                            \
    ({                                                                         \
        __auto_type _m = _MAX_6(a, b, c, d, e, f);                             \
        __auto_type _g = (g);                                                  \
        _m > _g ? _m : _g;                                                     \
    })

#define _MAX_8(a, b, c, d, e, f, g, h)                                         \
    ({                                                                         \
        __auto_type _m = _MAX_7(a, b, c, d, e, f, g);                          \
        __auto_type _h = (h);                                                  \
        _m > _h ? _m : _h;                                                     \
    })

#define MAX(...) _DISPATCH(_MAX, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
