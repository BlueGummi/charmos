#include <drivers/nvme.h>
#include <log.h>

#define NVME_CMD_TIMEOUT_MS 2000    // Normal command timeout
#define NVME_ADMIN_TIMEOUT_MS 5000  // Admin commands
#define NVME_RESET_TIMEOUT_MS 30000 // Controller reset or format NVM
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(nvme_queue, lock);
#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))
#define THIS_QID(nvme) (1 + (smp_core_id() % (nvme->queue_count)))

LOG_SITE_EXTERN(nvme);
LOG_HANDLE_EXTERN(nvme);
#define nvme_log(log_level, fmt, ...)                                          \
    log(LOG_SITE(nvme), LOG_HANDLE(nvme), log_level, fmt, ##__VA_ARGS__)

#define NVME_COMPLETION_PHASE(cpl) ((cpl)->status & 0x1)
#define NVME_COMPLETION_STATUS(cpl) (((cpl)->status >> 1) & 0x7FFF)

#define NVME_DOORBELL_BASE 0x1000

#define NVME_OP_ADMIN_DELETE_IOSQ 0x0
#define NVME_OP_ADMIN_CREATE_IOSQ 0x1

#define NVME_OP_ADMIN_GET_LOG_PG 0x2

#define NVME_OP_ADMIN_DELETE_IOCQ 0x4
#define NVME_OP_ADMIN_CREATE_IOCQ 0x5

#define NVME_OP_ADMIN_IDENT 0x6
#define NVME_OP_ADMIN_SET_FEATS 0x9
#define NVME_OP_ADMIN_GET_FEATS 0x10

#define NVME_OP_IO_READ 0x02
#define NVME_OP_IO_WRITE 0x01

#define NVME_STATUS_CONFLICTING_ATTRIBUTES 0x80
#define NVME_STATUS_INVALID_PROT_INFO 0x81

bool nvme_read_sector_async(struct generic_disk *disk,
                            struct nvme_request *req);

bool nvme_write_sector_async(struct generic_disk *disk,
                             struct nvme_request *req);

static inline enum workqueue_error nvme_work_enqueue(struct nvme_device *dev,
                                                     struct work *work) {
    return workqueue_enqueue(dev->workqueue, work);
}
