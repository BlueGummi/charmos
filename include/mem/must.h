/* @title: Boot OOM */
#pragma once
#include <console/panic.h>
#include <global.h>
#include <log.h>

#define must(expr)                                                             \
    ({                                                                         \
        if (global.current_bootstage >= BOOTSTAGE_COMPLETE)                    \
            log_msg_once(LOG_WARN, "must() invoked after boot");               \
        typeof(expr) _p_ = (expr);                                             \
        if (cc_unlikely(!_p_))                                                 \
            panic("OOM: %s == NULL, bootstage: %s", #expr,                     \
                  bootstage_str[global.current_bootstage]);                    \
        _p_;                                                                   \
    })

#define must_valloc(...) must(valloc(__VA_ARGS__))
#define must_vrealloc(...) must(vrealloc(__VA_ARGS__))
