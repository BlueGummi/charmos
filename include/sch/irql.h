#pragma once
enum irql {
    IRQL_PASSIVE_LEVEL = 0,  /* Normal execution */
    IRQL_APC_LEVEL = 1,      /* Allow only high interrupts */
    IRQL_DISPATCH_LEVEL = 2, /* Allow higher than DPC interrupts */
    IRQL_DEVICE_LEVEL = 3,   /* Device interrupts */
    IRQL_HIGH_LEVEL = 4      /* All interrupts masked */
};

enum irql irql_raise(enum irql new_level);
void irql_lower(enum irql old_level);
