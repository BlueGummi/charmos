/* Scheduler load balancing policy */

#include <smp/domain.h>

#include "internal.h"

uint64_t scheduler_find_idle_core_in_domain(void) {
    struct core_domain *domain = core_domain_local();
    struct core *iter;

    core_domain_for_each(domain, iter) {
        
    }
}
