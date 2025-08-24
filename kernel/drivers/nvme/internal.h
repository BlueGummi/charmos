#include <drivers/nvme.h>
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(nvme_queue, lock);
