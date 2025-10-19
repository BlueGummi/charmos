/* Scheduler load balancing policy */

#include <smp/domain.h>

#include "internal.h"

uint64_t scheduler_find_idle_core_in_domain(void) {
    struct domain *domain = domain_local();
    struct core *iter;

    domain_for_each(domain, iter) {
        
    }
}
