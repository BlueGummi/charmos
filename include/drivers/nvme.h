#pragma once
#include <block/generic.h>
#include <block/sched.h>
#include <mem/vmm.h>
#include <sch/thread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>

#define NVME_CMD_TIMEOUT_MS 2000    // Normal command timeout
#define NVME_ADMIN_TIMEOUT_MS 5000  // Admin commands
#define NVME_RESET_TIMEOUT_MS 30000 // Controller reset or format NVM

#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))
#define THIS_QID(nvme) (1 + (get_this_core_id() % (nvme->queue_count - 1)))

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

struct nvme_cc {
    union {
        uint32_t raw;
        struct {
            uint32_t en : 1;
            uint32_t __reserved0 : 3;
            uint32_t css : 3;
            uint32_t mps : 4;
            uint32_t ams : 3;
            uint32_t shn : 2;
            uint32_t iosqes : 4;
            uint32_t iocqes : 4;
            uint32_t __reserved1 : 8;
        };
    };
} __attribute__((packed));
_Static_assert(sizeof(struct nvme_cc) == sizeof(uint32_t),
               "nvme_cc != sizeof(uint64_t)");

struct nvme_regs {
    uint32_t cap_lo;
    uint32_t cap_hi;
    uint32_t version;
    uint32_t intms;
    uint32_t intmc;
    struct nvme_cc cc;
    uint32_t nssr;
    uint32_t csts;
    uint32_t reserved1;
    uint32_t aqa;
    uint32_t asq_lo;
    uint32_t asq_hi;
    uint32_t acq_lo;
    uint32_t acq_hi;
    uint32_t reserved4[1018];
} __attribute__((aligned));

struct nvme_bio_data {
    uint64_t *prps;
    uint64_t prp_count;    // current number of PRPs
    uint64_t prp_capacity; // allocated capacity
    struct bio_request *coalescee;
};

struct nvme_request {
    uint32_t qid;
    uint64_t lba;
    void *buffer;
    uint64_t size;
    uint64_t sector_count;
    bool write;

    volatile bool done;
    volatile uint16_t status;
    volatile int remaining_parts;

    void (*on_complete)(struct nvme_request *);
    struct nvme_bio_data *bio_data;
    struct thread *waiter;

    void *user_data;

    struct nvme_request *next;
};

struct nvme_queue {
    struct nvme_command *sq;    // Submission queue (virtual)
    struct nvme_completion *cq; // Completion queue (virtual)
    uint64_t sq_phys;           // Submission queue physical address
    uint64_t cq_phys;           // Completion queue physical address

    uint16_t sq_tail;  // Tail index for submission
    uint16_t cq_head;  // Head index for completion
    uint16_t sq_depth; // Queue depth (entries)
    uint16_t cq_depth; // Queue depth (entries)
    uint8_t cq_phase;  // Phase bit for completion
    uint32_t *sq_db;
    uint32_t *cq_db;

    uint16_t outstanding;

    struct spinlock lock;
};

struct nvme_waiting_requests {
    struct spinlock lock;
    struct nvme_request *head;
    struct nvme_request *tail;
};

struct nvme_device {
    struct nvme_regs *regs;
    uint64_t cap;
    uint32_t version;
    uint32_t doorbell_stride;
    uint32_t page_size;
    uint32_t *admin_sq_db;
    uint32_t *admin_cq_db;

    struct nvme_command *admin_sq;
    struct nvme_completion *admin_cq;
    uint64_t admin_sq_phys;
    uint64_t admin_cq_phys;

    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint16_t admin_q_depth;
    uint8_t admin_cq_phase;

    /* Array of pointers to queues */
    struct nvme_queue **io_queues;

    /* 2D array of pointers to io requests
     * in queues and queue entries */
    struct nvme_request ***io_requests;
    struct nvme_waiting_requests waiting_requests;

    uint8_t *isr_index;
    uint32_t queue_count;

    uint32_t sector_size;
    uint64_t max_transfer_size;
    struct generic_disk *generic_disk;
};

struct nvme_identify {
    uint8_t data[PAGE_SIZE];
};

struct nvme_lbaf {
    uint16_t ms;    // Metadata size
    uint8_t lbads;  // LBA data size (log2 of sector size)
    uint8_t rp : 2; // Relative performance
    uint8_t reserved : 6;
} __attribute__((packed));

struct nvme_identify_namespace {
    uint64_t nsze;  // Namespace Size
    uint64_t ncap;  // Namespace Capacity
    uint64_t nuse;  // Namespace Utilization
    uint8_t nsfeat; // Namespace Features
    uint8_t nlbaf;  // Number of LBA formats
    uint8_t flbas;  // Formatted LBA Size
    uint8_t mc;     // Metadata Capabilities
    uint8_t dpc;    // End-to-end Data Protection Capabilities
    uint8_t dps;    // End-to-end Data Protection Type Settings
    uint8_t nmic; // Namespace Multipath I/O and Namespace Sharing Capabilities
    uint8_t rescap;     // Reservation Capabilities
    uint8_t fpi;        // Format Progress Indicator
    uint8_t dlfeat;     // Deallocate Logical Block Features
    uint16_t nawun;     // Namespace Atomic Write Unit Normal
    uint16_t nawupf;    // Namespace Atomic Write Unit Power Fail
    uint16_t nacwu;     // Namespace Atomic Compare & Write Unit
    uint16_t nabsn;     // Namespace Atomic Boundary Size Normal
    uint16_t nabo;      // Namespace Atomic Boundary Offset
    uint16_t nabspf;    // Namespace Atomic Boundary Size Power Fail
    uint16_t noiob;     // Namespace Optimal IO Boundary
    uint64_t nvmcap[2]; // Namespace NVM Capacity
    uint16_t npwg;
    uint16_t npwa;
    uint16_t npdg;
    uint16_t npda;
    uint16_t nows;
    uint16_t mssrl;
    uint32_t mcl;
    uint8_t msrc;
    uint8_t reserved0[11];
    uint32_t adagrpid;
    uint8_t reserved1[3];
    uint8_t nsattr;
    uint16_t nvmsetid;
    uint16_t endgid;
    uint64_t nguid[2];
    uint64_t eui64;
    struct nvme_lbaf lbaf[64]; // LBA format descriptions
    uint8_t vendor_specific[3712];
} __attribute__((packed));
_Static_assert(sizeof(struct nvme_identify_namespace) == 0x1000, "");

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
} __attribute__((packed));

#define NVME_COMPLETION_PHASE(cpl) ((cpl)->status & 0x1)
#define NVME_COMPLETION_STATUS(cpl) (((cpl)->status >> 1) & 0x7FFF)
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02
#define nvme_info(lvl, fmt, ...) k_info("NVMe", lvl, fmt, ##__VA_ARGS__)

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

uint16_t nvme_submit_admin_cmd(struct nvme_device *nvme,
                               struct nvme_command *cmd, uint32_t *);
uint32_t nvme_set_num_queues(struct nvme_device *nvme, uint16_t desired_sq,
                             uint16_t desired_cq);

void nvme_submit_io_cmd(struct nvme_device *nvme, struct nvme_command *cmd,
                        uint32_t qid, struct nvme_request *req);

uint8_t *nvme_identify_controller(struct nvme_device *nvme);
uint8_t *nvme_identify_namespace(struct nvme_device *nvme, uint32_t nsid);
void nvme_enable_controller(struct nvme_device *nvme);
void nvme_setup_admin_queues(struct nvme_device *nvme);
void nvme_alloc_admin_queues(struct nvme_device *nvme);
void nvme_alloc_io_queues(struct nvme_device *nvme, uint32_t qid);
struct nvme_device *nvme_discover_device(uint8_t bus, uint8_t slot,
                                         uint8_t func);
struct generic_disk *nvme_create_generic(struct nvme_device *nvme);
void nvme_print_identify(const struct nvme_identify_controller *ctrl);
void nvme_print_namespace(const struct nvme_identify_namespace *ns);

bool nvme_read_sector_async(struct generic_disk *disk,
                            struct nvme_request *req);

bool nvme_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                      uint16_t cnt);

bool nvme_write_sector_async(struct generic_disk *disk,
                             struct nvme_request *req);

bool nvme_write_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                       uint16_t cnt);

bool nvme_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt);

bool nvme_read_sector_async_wrapper(struct generic_disk *disk,
                                    struct nvme_request *req);

bool nvme_write_sector_async_wrapper(struct generic_disk *disk,
                                     struct nvme_request *req);

bool nvme_send_nvme_req(struct generic_disk *d, struct nvme_request *r);

bool nvme_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt);

void nvme_isr_handler(void *ctx, uint8_t vector, void *rsp);

bool nvme_submit_bio_request(struct generic_disk *disk,
                             struct bio_request *bio);

bool nvme_should_coalesce(struct generic_disk *disk,
                          const struct bio_request *a,
                          const struct bio_request *b);

void nvme_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                      struct bio_request *from);

void nvme_reorder(struct generic_disk *disk);
