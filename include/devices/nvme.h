#pragma once
#include <stdint.h>
#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))

struct nvme_command {
    uint8_t opc;
    uint8_t fuse;
    uint16_t cid;
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
} __attribute__((packed));

struct nvme_completion {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

struct nvme_regs {
    volatile uint32_t cap_lo;          // 0x00
    volatile uint32_t cap_hi;          // 0x04
    volatile uint32_t version;         // 0x08
    volatile uint32_t intms;           // 0x0C
    volatile uint32_t intmc;           // 0x10
    volatile uint32_t cc;              // 0x14
    volatile uint32_t nssr;            // 0x18
    volatile uint32_t csts;            // 0x1C
    volatile uint32_t reserved1;       // 0x20 - 0x24
    volatile uint32_t aqa;             // 0x24
    volatile uint32_t asq_lo;          // 0x28
    volatile uint32_t asq_hi;          // 0x2C
    volatile uint32_t acq_lo;          // 0x30
    volatile uint32_t acq_hi;          // 0x34
    volatile uint32_t reserved4[1016]; // pad to 4KB total
} __attribute__((aligned));

struct nvme_device {
    struct nvme_regs *regs;
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
    uint8_t admin_cq_phase;
};

struct nvme_identify {
    uint8_t data[4096];
};

struct nvme_identify_controller {
    uint16_t vid;    // PCI Vendor ID
    uint16_t ssvid;  // Subsystem Vendor ID
    char sn[20];     // Serial Number (ASCII)
    char mn[40];     // Model Number (ASCII)
    char fr[8];      // Firmware Revision (ASCII)
    uint8_t rab;     // Recommended Arbitration Burst
    uint8_t ieee[3]; // IEEE OUI Identifier
    uint8_t mic;     // Multi-interface Capabilities
    uint8_t mdts;    // Maximum Data Transfer Size
    uint16_t cntlid; // Controller ID
    uint32_t ver;    // Version
    uint32_t rtd3r;  // RTD3 Resume Latency
    uint32_t rtd3e;  // RTD3 Entry Latency
    uint32_t oaes;   // Optional Admin Command Support
    uint32_t ctratt; // Controller Attributes
    uint8_t rsvd96[156];
    uint16_t oacs;       // Optional Admin Command Support (bit flags)
    uint8_t acl;         // Abort Command Limit
    uint8_t aerl;        // Asynchronous Event Request Limit
    uint8_t frmw;        // Firmware Updates
    uint8_t lpa;         // Log Page Attributes
    uint8_t elpe;        // Error Log Page Entries
    uint8_t npss;        // Number of Power States Support
    uint8_t avscc;       // Admin Vendor Specific Command Configuration
    uint8_t apsta;       // Autonomous Power State Transition Attributes
    uint16_t wctemp;     // Warning Composite Temperature Threshold
    uint16_t cctemp;     // Critical Composite Temperature Threshold
    uint16_t mtfa;       // Maximum Time for Firmware Activation
    uint32_t hmpre;      // Host Memory Buffer Preferred Size
    uint32_t hmmin;      // Host Memory Buffer Minimum Size
    uint64_t tnvmcap[2]; // Total NVM Capacity
    uint64_t unvmcap[2]; // Unallocated NVM Capacity
    uint32_t rpmbs;      // Replay Protected Memory Block Support
    uint16_t edstt;      // Extended Device Self-test Time
    uint8_t dsto;        // Device Self-test Options
    uint8_t fwug;        // Firmware Update Granularity
    uint16_t kas;        // Keep Alive Support
    uint16_t hctma;      // Host Controlled Thermal Management Attributes
    uint16_t mntmt;      // Minimum Thermal Management Temperature
    uint16_t mxtmt;      // Maximum Thermal Management Temperature
    uint32_t sanicap;    // Sanitize Capabilities
    uint8_t rsvd228[180];
    uint8_t sqes; // Submission Queue Entry Size
    uint8_t cqes; // Completion Queue Entry Size
    // TODO: there is more but me lazy and dont need it
};

#define NVME_COMPLETION_PHASE(cpl) ((cpl)->status & 0x1)
#define NVME_COMPLETION_STATUS(cpl) (((cpl)->status >> 1) & 0x7FFF)
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_ADMIN_GET_FEATURES 0x0A

void nvme_scan_pci();
