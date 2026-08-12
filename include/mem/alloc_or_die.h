/* @title: Boot OOM */
#pragma once
#include <console/panic.h>
#include <global.h>
#include <log.h>

#define alloc_or_die(expr)                                                     \
    ({                                                                         \
        if (global.current_bootstage >= BOOTSTAGE_COMPLETE)                    \
            log_warn_once("alloc_or_die invoked after boot");                  \
        __typeof__(expr) _p_ = (expr);                                         \
        if (unlikely(!_p_))                                                    \
            panic("OOM: %s == NULL, bootstage: %s", #expr,                     \
                  bootstage_str[global.current_bootstage]);                    \
        _p_;                                                                   \
    })

#define kmalloc_or_die(...) alloc_or_die(kmalloc(__VA_ARGS__))
#define krealloc_or_die(...) alloc_or_die(krealloc(__VA_ARGS__))
