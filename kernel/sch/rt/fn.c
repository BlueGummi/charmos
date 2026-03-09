#include <string.h>

#include "internal.h"
#include "sch/internal.h"

enum rt_scheduler_error rt_ext_fn_exec(struct rt_scheduler_static *rts,
                                       char *name, uint32_t id, uintptr_t a,
                                       uintptr_t b) {
    struct rt_ext_fn *fns = rts->ext_fns;
    struct rt_ext_fn *found = NULL;
    for (size_t i = 0; i < rts->num_ext_fns; i++) {
        if ((name && strcmp(fns[i].name, name) == 0) || (fns[i].id == id)) {
            found = &fns[i];
            break;
        }
    }

    if (!found)
        return RT_SCHEDULER_ERR_NOT_FOUND;

    /* Got it */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    smp_core_scheduler()->rt->active_flags = found->flags;
    found->fn(a, b);
    smp_core_scheduler()->rt->active_flags = 0;

    irql_lower(irql);
    return RT_SCHEDULER_ERR_OK;
}
