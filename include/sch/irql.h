#pragma once
enum irql {
    IRQL_PASSIVE_LEVEL = 0,  /* Normal execution */
    IRQL_APC_LEVEL = 1,      /* Allow only high interrupts */
    IRQL_DISPATCH_LEVEL = 2, /* Allow higher than DPC interrupts */
    IRQL_DEVICE_LEVEL = 3,   /* Device interrupts */
    IRQL_HIGH_LEVEL = 4      /* All interrupts masked */
};

static inline const char *irql_to_str(enum irql level) {
    switch (level) {
    case IRQL_PASSIVE_LEVEL: return "PASSIVE LEVEL";
    case IRQL_APC_LEVEL: return "APC LEVEL";
    case IRQL_DISPATCH_LEVEL: return "DISPATCH LEVEL";
    case IRQL_DEVICE_LEVEL: return "DEVICE LEVEL";
    case IRQL_HIGH_LEVEL: return "HIGH LEVEL";
    }
    return "UNKNOWN";
}

enum irql irql_raise(enum irql new_level);
void irql_lower(enum irql old_level);
