#pragma once
#include <stdint.h>
#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))

struct nvme_command {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct nvme_completion {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
};

struct nvme_device {
    uint32_t *regs;
    uint64_t cap;
    uint32_t version;
    uint32_t doorbell_stride;
    uint32_t page_size;

    struct nvme_command *admin_sq;
    struct nvme_completion *admin_cq;
    uint64_t admin_sq_phys;
    uint64_t admin_cq_phys;

    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint16_t admin_q_depth;
};

struct nvme_identify {
    uint8_t data[4096];
};

#define NVME_REG_CAP 0x0000  // Controller capabilities
#define NVME_REG_VER 0x0008  // Version
#define NVME_REG_CC 0x0014   // Controller Configuration
#define NVME_REG_CSTS 0x001C // Controller Status
#define NVME_REG_AQA 0x0024  // Admin Queue Attributes
#define NVME_REG_ASQ 0x0028  // Admin Submission Queue Base Address
#define NVME_REG_ACQ 0x0030  // Admin Completion Queue Base Address
#define NVME_DOORBELL_CQ_HEAD(dev, qid) ((((qid) * 2 + 1) * dev->doorbell_stride) * 4)
#define NVME_DOORBELL_SQ_TAIL(dev, qid) ((((qid) * 2 + 0) * dev->doorbell_stride) * 4)


#define NVME_COMPLETION_PHASE(cpl) ((cpl)->status & 0x1)
#define NVME_COMPLETION_STATUS(cpl) (((cpl)->status >> 1) & 0x7FFF)
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02
#define NVME_ADMIN_IDENTIFY 0x06

void nvme_scan_pci();
