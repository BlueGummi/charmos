#pragma once
#include <stdint.h>
#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))

struct nvme_command {
    uint8_t opc;      // Opcode
    uint8_t fuse : 2; // Fused operation
    uint8_t rsvd1 : 6;
    uint16_t cid; // Command identifier

    uint32_t nsid; // Namespace ID

    uint64_t rsvd2;
    uint64_t mptr; // Metadata pointer
    uint64_t prp1; // PRP entry 1
    uint64_t prp2; // PRP entry 2

    uint32_t cdw[6]; // Command-specific
};

struct nvme_completion {
    uint32_t dw0;
    uint32_t rsvd;

    uint16_t sq_head; // SQ head pointer
    uint16_t sq_id;   // SQ ID
    uint16_t cid;     // Command identifier
    uint16_t status;  // Status and phase tag (bit 0)
};

struct nvme_device {
    volatile uint32_t *regs;
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

#define NVME_REG_CAP 0x0000  // Controller capabilities
#define NVME_REG_VER 0x0008  // Version
#define NVME_REG_CC 0x0014   // Controller Configuration
#define NVME_REG_CSTS 0x001C // Controller Status
#define NVME_REG_AQA 0x0024  // Admin Queue Attributes
#define NVME_REG_ASQ 0x0028  // Admin Submission Queue Base Address
#define NVME_REG_ACQ 0x0030  // Admin Completion Queue Base Address
#define NVME_REG_SQTDBL(n, stride)                                             \
    (0x1000 + (2 * (n) * (stride))) // Submission Queue Tail Doorbell
#define NVME_REG_CQHDBL(n, stride)                                             \
    (0x1000 + (2 * (n) + 1) * (stride)) // Completion Queue Head Doorbell
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02

void nvme_scan_pci();
