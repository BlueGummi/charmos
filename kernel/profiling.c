#include <profiling.h>

extern struct profiling_entry __skernel_profiling_data[];
extern struct profiling_entry __ekernel_profiling_data[];

#ifdef PROFILING_ENABLED
void profiling_init(void) {
    list_head_init(&global.profiling_list_head);
    struct profiling_entry *start = __skernel_profiling_data;
    struct profiling_entry *end = __ekernel_profiling_data;

    for (struct profiling_entry *pe = start; pe < end; pe++)
        list_head_push_front(&global.profiling_list_head, &pe->list_node);
}

void profiling_log_all(void) {
    struct profiling_entry *start = __skernel_profiling_data;
    struct profiling_entry *end = __ekernel_profiling_data;

    for (struct profiling_entry *pe = start; pe < end; pe++)
        (pe->log)(pe->data);
}
#else
void profiling_init(void) { /* Nothing */ }
void profiling_log_all(void) { /* Nothing */ }
#endif
