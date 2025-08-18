#include <drivers/nvme.h>

static inline bool nvme_queue_lock(struct nvme_queue *q) {
    return spin_lock(&q->lock);
}

static inline void nvme_queue_unlock(struct nvme_queue *q, bool iflag) {
    spin_unlock(&q->lock, iflag);
}
