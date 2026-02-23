/* @title: e1000 */
#include <compiler.h>
#include <drivers/pci.h>
#include <log.h>
#include <stdbool.h>
#include <stdint.h>
#pragma once
// Register offsets
#define E1000_REG_CTRL 0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_TCTL 0x0400
#define E1000_REG_TDBAL 0x03800
#define E1000_REG_TDBAH 0x03804
#define E1000_REG_TDLEN 0x03808
#define E1000_REG_TDH 0x03810
#define E1000_REG_TDT 0x03818

#define E1000_REG_RCTL 0x0100
#define E1000_REG_RDBAL 0x02800
#define E1000_REG_RDBAH 0x02804
#define E1000_REG_RDLEN 0x02808
#define E1000_REG_RDH 0x02810
#define E1000_REG_RDT 0x02818

#define E1000_REG_IMS 0x00D0
#define E1000_REG_ICR 0x00C0

// Transmit Control Register
#define E1000_TCTL_EN (1 << 1)
#define E1000_TCTL_PSP (1 << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

// Receive Control Register
#define E1000_RCTL_EN (1 << 1)
#define E1000_RCTL_BAM (1 << 15)
#define E1000_RCTL_SECRC (1 << 26)

#define E1000_NUM_RX_DESC 64
#define E1000_NUM_TX_DESC 64

struct e1000_device {
    uint32_t *regs; // MMIO virtual base

    // TX
    struct e1000_tx_desc *tx_descs;
    uintptr_t tx_descs_phys;
    uint32_t tx_tail;
    void *tx_buffers[E1000_NUM_TX_DESC]; // one per desc

    // RX
    struct e1000_rx_desc *rx_descs;
    uintptr_t rx_descs_phys;
    uint32_t rx_tail;
    void *rx_buffers[E1000_NUM_RX_DESC];

    // PCI location
    uint8_t bus, device, function;
};

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    union {
        uint8_t cmd;
        struct {
            uint8_t eop : 1;
            uint8_t ifcs : 1;
            uint8_t ic : 1;
            uint8_t rs : 1;
            uint8_t rsvd : 1;
            uint8_t dext : 1;
            uint8_t vlei : 1;
            uint8_t ide : 1;
        };
    };
    union {
        uint8_t status;
        struct {
            uint8_t dd : 1;
            uint8_t ec : 1;
            uint8_t lc : 1;
            uint8_t rsvd0 : 5;
        };
    };
    uint8_t css;
    uint16_t special;
} __packed;

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    union {
        uint8_t status;
        struct {
            uint8_t dd : 1;
            uint8_t eop : 1;
            uint8_t ixsm : 1;
            uint8_t vp : 1;
            uint8_t rsvd : 4;
        };
    };
    uint8_t errors;
    uint16_t special;
} __packed;

// Ethernet header (14 bytes)
struct eth_hdr {
    uint8_t dest[6];
    uint8_t src[6];
    uint16_t ethertype;
} __packed;

// IPv4 header (20 bytes)
struct ipv4_hdr {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} __packed;

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __packed;

#define E1000_TXD_CMD_EOP (1 << 0)
#define E1000_TXD_CMD_IFCS (1 << 1)
#define E1000_TXD_CMD_RS (1 << 3)
#define E1000_TXD_STAT_DD (1 << 0)

#define E1000_RXD_STAT_DD (1 << 0)
#define E1000_RXD_STAT_EOP (1 << 1)
#define E1000_CTRL_RST (1 << 26) // Software reset

#define E1000_BAR_INDEX 0
#define PCI_BAR0 0x10

#define E1000_RX_BUF_SIZE 2048

bool e1000_init(struct pci_device *dev, struct e1000_device *out);
#define E1000_MAX_TX_PACKET_SIZE 1518 // Standard Ethernet frame

LOG_SITE_EXTERN(e1000);
LOG_HANDLE_EXTERN(e1000);
#define e1000_log(log_level, fmt, ...)                                         \
    log(LOG_SITE(e1000), LOG_HANDLE(e1000), log_level, fmt, ##__VA_ARGS__)
