#include <profiling.h>

extern struct profiling_entry __skernel_profiling_data[];
extern struct profiling_entry __ekernel_profiling_data[];

#ifdef PROFILING_ENABLED
void profiling_init(void) {
    INIT_LIST_HEAD(&global.profiling_list_head);
    struct profiling_entry *start = __skernel_profiling_data;
    struct profiling_entry *end = __ekernel_profiling_data;

    for (struct profiling_entry *pe = start; pe < end; pe++)
        list_add(&pe->list_node, &global.profiling_list_head);
}

void profiling_log_all(void) {
    struct profiling_entry *start = __skernel_profiling_data;
    struct profiling_entry *end = __ekernel_profiling_data;

    for (struct profiling_entry *pe = start; pe < end; pe++)
        pe->log(pe->data);
}
#else
void profiling_init(void) { /* Nothing */ }
void profiling_log_all(void) { /* Nothing */ }
#endif
