#include <console/printf.h>
#include <drivers/nvme.h>
#include <log.h>
#include <math/div.h>
#include <mem/vmm.h>

#define NVME_CMD_TIMEOUT_MS 2000    // Normal command timeout
#define NVME_ADMIN_TIMEOUT_MS 5000  // Admin commands
#define NVME_RESET_TIMEOUT_MS 30000 // Controller reset or format NVM
#define THIS_QID(nvme) (1 + (smp_core_id() % (nvme->queue_count)))

LOG_SITE_EXTERN(nvme);
LOG_HANDLE_EXTERN(nvme);
#define nvme_log(log_level, fmt, ...)                                          \
    log(LOG_SITE(nvme), LOG_HANDLE(nvme), log_level, fmt, ##__VA_ARGS__)

#define NVME_COMPLETION_PHASE(cpl) ((cpl)->status & 0x1)
#define NVME_COMPLETION_STATUS(cpl) (((cpl)->status >> 1) & 0x7FFF)

#define NVME_DOORBELL_BASE 0x1000

#define NVME_PRPS_PER_PAGE (PAGE_SIZE / sizeof(uint64_t))

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

/* We funnel addresses through this so we can catch bad ones */
static inline void nvme_check_dma_addr(uint64_t phys, const char *what) {
    if (phys == (uint64_t) -1 || phys == 0)
        panic("NVMe: %s has no physical mapping (0x%lx)", what, phys);

    if (vmm_phys_is_kernel_text(phys))
        panic("NVMe: %s aims at kernel text (phys 0x%lx)", what, phys);
}

bool nvme_read_sector_async(struct block_device *disk,
                            struct nvme_request *req);

bool nvme_write_sector_async(struct block_device *disk,
                             struct nvme_request *req);

static inline enum workqueue_error nvme_work_enqueue(struct nvme_device *dev,
                                                     struct work *work) {
    return workqueue_enqueue(dev->workqueue, work);
}
