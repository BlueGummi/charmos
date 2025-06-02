#pragma once
#include <devices/generic_disk.h>
#include <stdbool.h>
#include <stdint.h>

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
#define AHCI_DEV_SATAPI 2
#define AHCI_DEV_SEMB 3
#define AHCI_DEV_PM 4

#define AHCI_CMD_TABLE_FIS_SIZE 64
#define AHCI_CMD_TABLE_ATAPI_SIZE 16
#define AHCI_MAX_PRDT_ENTRIES 65535

#define AHCI_CMD_ST (1 << 0)  // Start
#define AHCI_CMD_SUD (1 << 1) // Spin-Up Device
#define AHCI_CMD_FRE (1 << 4) // FIS Receive Enable
#define AHCI_CMD_FR (1 << 14) // FIS Receive Running
#define AHCI_CMD_CR (1 << 15) // Command List Running
#define AHCI_CMD_CLO (1 << 3) // Command List Override

#define AHCI_PORT_IPM_ACTIVE 1
#define AHCI_PORT_DET_PRESENT 3

#define AHCI_CMD_FLAGS_WRITE (1 << 6)
#define AHCI_CMD_FLAGS_PRDTL 1

#define FIS_TYPE_REG_H2D 0x27
#define FIS_REG_CMD 0x80
#define LBA_MODE 0x40
#define CONTROL_BIT 0x80

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

volatile struct ahci_port {
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

struct ahci_device {
    uint8_t type;           // Device type
    uint8_t port;           // Port number
    bool implemented;       // Is this port implemented?
    bool active;            // Is this port active?
    uint32_t signature;     // Device signature
    uint32_t sectors;       // Total sectors (for disks)
    uint16_t sector_size;   // Sector size in bytes
    struct ahci_port *regs; // Pointer to port registers
};

volatile struct ahci_controller {
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
    struct ahci_port ports[1];

} __attribute__((packed));

struct ahci_cmd_header {
    uint16_t flags; // Bitfield: 0x1 = write, 0x5 = prefetch, etc.
    uint16_t prdtl; // Physical region descriptor table length
    uint32_t prdbc; // Physical region descriptor byte count
    uint32_t ctba;  // Command table base address
    uint32_t ctbau; // Upper 32 bits of CTBA
    uint32_t reserved[4];
} __attribute__((packed));

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
    struct ahci_prdt_entry prdt_entry[];
} __attribute__((packed));

void ahci_print_ctrlr(struct ahci_controller *ctrl);
void ahci_discover(struct ahci_controller *ctrl);
