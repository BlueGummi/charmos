#pragma once
#include <devices/generic_disk.h>
#include <s_assert.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stdint.h>

#define AHCI_CMD_TIMEOUT_MS 5000    // Data commands (read/write)
#define AHCI_IDENT_TIMEOUT_MS 10000 // Identify, flush cache, etc.
#define AHCI_RESET_TIMEOUT_MS 30000 // Full controller reset or COMRESET

#define AHCI_CAP 0x00       // Host Capabilities
#define AHCI_GHC 0x04       // Global Host Control
#define AHCI_IS 0x08        // Interrupt Status
#define AHCI_PI 0x0C        // Ports Implemented
#define AHCI_VS 0x10        // AHCI Version
#define AHCI_CCC_CTL 0x14   // Command Completion Coalescing Control
#define AHCI_CCC_PORTS 0x18 // CCC Ports
#define AHCI_EM_LOC 0x1C    // Enclosure Management Location
#define AHCI_EM_CTL 0x20    // Enclosure Management Control
#define AHCI_CAP2 0x24      // Host Capabilities Extended
#define AHCI_BOHC 0x28      // BIOS/OS Handoff Control and Status

#define AHCI_PORT_BASE 0x100 // Base offset for port registers
#define AHCI_PORT_SIZE 0x80  // Size of each port register set

#define AHCI_MAX_PORTS 32

#define AHCI_PORT_CLB 0x00  // Command List Base Address
#define AHCI_PORT_CLBU 0x04 // Command List Base Address Upper
#define AHCI_PORT_FB 0x08   // FIS Base Address
#define AHCI_PORT_FBU 0x0C  // FIS Base Address Upper
#define AHCI_PORT_IS 0x10   // Interrupt Status
#define AHCI_PORT_IE 0x14   // Interrupt Enable
#define AHCI_PORT_CMD 0x18  // Command and Status
#define AHCI_PORT_TFD 0x20  // Task File Data
#define AHCI_PORT_SIG 0x24  // Signature
#define AHCI_PORT_SSTS 0x28 // SATA Status
#define AHCI_PORT_SCTL 0x2C // SATA Control
#define AHCI_PORT_SERR 0x30 // SATA Error
#define AHCI_PORT_SACT 0x34 // SATA Active
#define AHCI_PORT_CI 0x38   // Command Issue
#define AHCI_PORT_SNTF 0x3C // SATA Notification

#define AHCI_DEV_NULL 0
#define AHCI_DEV_SATA 1
#define AHCI_DEV_BUSY (1 << 30)
#define AHCI_DEV_DRDY 0x40
#define AHCI_DEV_SATAPI 2
#define AHCI_DEV_SEMB 3
#define AHCI_DEV_PM 4

#define AHCI_CMD_TABLE_FIS_SIZE 64
#define AHCI_CMD_TABLE_ATAPI_SIZE 16
#define AHCI_MAX_PRDT_ENTRIES 65535

#define AHCI_GHC_HR (1U << 0)
#define AHCI_GHC_AE (1U << 31)

#define AHCI_DET_NO_DEVICE 0x0
#define AHCI_DET_PRESENT 0x3

#define AHCI_IPM_NO_INTERFACE 0x0
#define AHCI_IPM_ACTIVE 0x1

#define AHCI_CMD_READ_DMA_EXT 0x25
#define AHCI_CMD_WRITE_DMA_EXT 0x35

#define AHCI_CMD_IDENTIFY 0xEC
#define AHCI_CMD_ST (1U << 0)  // Start
#define AHCI_CMD_SUD (1U << 1) // Spin-Up Device
#define AHCI_CMD_FRE (1U << 4) // FIS Receive Enable
#define AHCI_CMD_FR (1U << 14) // FIS Receive Running
#define AHCI_CMD_CR (1U << 15) // Command List Running
#define AHCI_CMD_CLO (1U << 3) // Command List Override

#define AHCI_PORT_IPM_ACTIVE 1
#define AHCI_PORT_DET_PRESENT 3

#define AHCI_CMD_FLAGS_WRITE (1 << 6)
#define AHCI_CMD_FLAGS_PRDTL 1

#define FIS_TYPE_REG_H2D 0x27
#define FIS_REG_CMD 0x80
#define LBA_MODE 0x40
#define CONTROL_BIT 0x80

#define AHCI_CMD_FLAGS_CFL_MASK 0x1F
#define AHCI_CMD_FLAGS_W_BIT 0x40

struct ahci_fis_reg_h2d {
    uint8_t fis_type; // 0x27
    uint8_t pmport : 4;
    uint8_t reserved1 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;

    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved2[4];
};

struct ahci_fis_reg_d2h {
    // DWORD 0
    uint8_t fis_type; // FIS_TYPE_REG_D2H

    uint8_t pmport : 4; // Port multiplier
    uint8_t rsv0 : 2;   // Reserved
    uint8_t i : 1;      // Interrupt bit
    uint8_t rsv1 : 1;   // Reserved

    uint8_t status; // Status register
    uint8_t error;  // Error register

    // DWORD 1
    uint8_t lba0;   // LBA low register, 7:0
    uint8_t lba1;   // LBA mid register, 15:8
    uint8_t lba2;   // LBA high register, 23:16
    uint8_t device; // Device register

    // DWORD 2
    uint8_t lba3; // LBA register, 31:24
    uint8_t lba4; // LBA register, 39:32
    uint8_t lba5; // LBA register, 47:40
    uint8_t rsv2; // Reserved

    // DWORD 3
    uint8_t countl;  // Count register, 7:0
    uint8_t counth;  // Count register, 15:8
    uint8_t rsv3[2]; // Reserved

    // DWORD 4
    uint8_t rsv4[4]; // Reserved
};

struct ahci_prdt_entry {
    uint32_t dba;  // Data base address
    uint32_t dbau; // Upper 32-bits of DBA
    uint32_t reserved;
    uint32_t dbc : 22; // Byte count (0-based)
    uint32_t reserved2 : 9;
    uint32_t i : 1; // Interrupt on completion
} __attribute__((packed));

struct ahci_cmd_table {
    uint8_t cfis[AHCI_CMD_TABLE_FIS_SIZE];   // Command FIS (host to device)
    uint8_t acmd[AHCI_CMD_TABLE_ATAPI_SIZE]; // ATAPI command
    uint8_t reserved[48];
    struct ahci_prdt_entry prdt_entry[]; // up to 65535
} __attribute__((packed));

struct ahci_full_port {
    struct ahci_port *port;
    void *cmd_list_base;
    void *fis;
    struct ahci_cmd_table **cmd_tables;
    struct ahci_cmd_header **cmd_hdrs;
};

struct ahci_port {
    uint32_t clb;       // Command List Base (lower 32 bits)
    uint32_t clbu;      // Command List Base (upper 32 bits)
    uint32_t fb;        // FIS Base (lower 32 bits)
    uint32_t fbu;       // FIS Base (upper 32 bits)
    uint32_t is;        // Interrupt Status
    uint32_t ie;        // Interrupt Enable
    uint32_t cmd;       // Command and Status
    uint32_t rsv0;      // Reserved
    uint32_t tfd;       // Task File Data
    uint32_t sig;       // Signature
    uint32_t ssts;      // SATA Status
    uint32_t sctl;      // SATA Control
    uint32_t serr;      // SATA Error
    uint32_t sact;      // SATA Active
    uint32_t ci;        // Command Issue
    uint32_t sntf;      // SATA Notification
    uint32_t fbs;       // FIS-based Switching
    uint32_t rsv1[11];  // 0x44 ~ 0x6F, Reserved
    uint32_t vendor[4]; // 0x70 ~ 0x7F, vendor specific
} __attribute__((packed));

// one controller
struct ahci_device {
    uint8_t type;         // Device type
    uint32_t signature;   // Device signature
    uint32_t sectors;     // Total sectors (for disks)
    uint16_t sector_size; // Sector size in bytes
    struct ahci_controller *ctrl;
    uint64_t port_count;

    struct thread *io_waiters[AHCI_MAX_PORTS][32];
    uint16_t io_statuses[AHCI_MAX_PORTS][32];

    struct ahci_request *io_requests[AHCI_MAX_PORTS][32];

    uint8_t irq_num;
    struct ahci_full_port regs[32]; // Pointer to port registers
};

// one disk
struct ahci_disk {
    struct ahci_device *device;
    uint32_t port;
};

struct ahci_controller {
    uint32_t cap;       // Host Capabilities
    uint32_t ghc;       // Global Host Control
    uint32_t is;        // Interrupt Status
    uint32_t pi;        // Ports Implemented
    uint32_t vs;        // Version
    uint32_t ccc_ctl;   // Command Completion Coalescing Control
    uint32_t ccc_ports; // CCC Ports
    uint32_t em_loc;    // Enclosure Management Location
    uint32_t em_ctl;    // Enclosure Management Control
    uint32_t cap2;      // Extended Host Capabilities
    uint32_t bohc;      // BIOS/OS Handoff Control
    uint8_t rsv[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
};

struct ahci_cmd_header {
    // DW0
    uint8_t cfl : 5; // Command FIS length in DWORDS, 2 ~ 16
    uint8_t a : 1;   // ATAPI
    uint8_t w : 1;   // Write, 1: H2D, 0: D2H
    uint8_t p : 1;   // Prefetchable

    uint8_t r : 1;    // Reset
    uint8_t b : 1;    // BIST
    uint8_t c : 1;    // Clear busy upon R_OK
    uint8_t rsv0 : 1; // Reserved
    uint8_t pmp : 4;  // Port multiplier port

    uint16_t prdtl; // Physical region descriptor table length in entries

    // DW1
    uint32_t prdbc; // Physical region descriptor byte count transferred

    // DW2, 3
    uint32_t ctba;  // Command table descriptor base address
    uint32_t ctbau; // Command table descriptor base address upper 32 bits

    // DW4 - 7
    uint32_t rsv1[4]; // Reserved
} __attribute__((packed));

STATIC_ASSERT((sizeof(struct ahci_cmd_header) == 32),
              "AHCI command header must be 28 bytes large!");

#define ahci_info(log_level, fmt, ...)                                         \
    k_info("AHCI", log_level, fmt, ##__VA_ARGS__)

struct ahci_request {
    uint32_t port;
    uint32_t slot;
    uint64_t lba;
    void *buffer;
    uint64_t size;
    uint64_t sector_count;
    bool write;

    volatile bool done;
    int status;

    void (*on_complete)(struct ahci_request *);
    void *user_data;
};

void ahci_discover(struct ahci_controller *ctrl);
uint32_t ahci_find_slot(struct ahci_port *port);
struct ahci_disk *ahci_setup_controller(struct ahci_controller *ctrl,
                                        uint32_t *d_cnt);
void ahci_identify(struct ahci_disk *disk);
void ahci_prepare_command(struct ahci_full_port *port, uint32_t slot,
                          bool write, uint8_t *buf, uint64_t size);
void ahci_setup_fis(struct ahci_cmd_table *cmd_tbl, uint8_t command,
                    bool is_atapi);

void ahci_send_command(struct ahci_disk *disk, struct ahci_full_port *port,
                       struct ahci_request *req);

struct ahci_disk *ahci_discover_device(uint8_t bus, uint8_t device,
                                       uint8_t function,
                                       uint32_t *out_disk_count);

bool ahci_write_sector(struct generic_disk *disk, uint64_t lba,
                       const uint8_t *in_buf, uint16_t cnt);
bool ahci_write_sector_async(struct generic_disk *disk, uint64_t lba,
                             const uint8_t *in_buf, uint16_t count,
                             struct ahci_request *req);

bool ahci_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *out_buf,
                      uint16_t cnt);
bool ahci_read_sector_async(struct generic_disk *disk, uint64_t lba,
                            uint8_t *buf, uint16_t count,
                            struct ahci_request *req);

bool ahci_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt);

bool ahci_read_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                    uint8_t *buf, uint64_t cnt,
                                    struct ahci_request *req);

bool ahci_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt);

bool ahci_write_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                     const uint8_t *buf, uint64_t cnt,
                                     struct ahci_request *req);

bool ahci_submit_bio_request(struct generic_disk *disk,
                             struct bio_request *bio);

struct generic_disk *ahci_create_generic(struct ahci_disk *disk);
void ahci_isr_handler(void *ctx, uint8_t vector, void *rsp);

#define AHCI_PORT_OFFSET(n) (0x100 + (n) * 0x80)

static inline struct ahci_port *ahci_get_port(struct ahci_device *dev, int n) {
    return (struct ahci_port *) ((uintptr_t) dev->ctrl + AHCI_PORT_OFFSET(n));
}
