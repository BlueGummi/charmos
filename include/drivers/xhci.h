#include <stdint.h>
#pragma once

// 5.3: XHCI Capability Registers
struct xhci_cap_regs {
    uint8_t cap_length;
    uint8_t reserved;
    uint16_t hci_version;
    uint32_t hcs_params1;
    uint32_t hcs_params2;
    uint32_t hcs_params3;
    uint32_t hcc_params1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hcc_params2;
} __attribute__((packed));

// 5.4: XHCI Operational Registers
struct xhci_op_regs {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint32_t reserved[2];
    uint32_t dnctrl;
    uint64_t crcr;
    uint32_t reserved2[4];
    uint64_t dcbaap;
    uint32_t config;
} __attribute__((packed));

struct xhci_trb { // Transfer Ring Buffer
    uint32_t dword0;
    uint32_t dword1;
    uint32_t dword2;
    uint32_t dword3;
};

struct xhci_ring {
    struct xhci_trb *trbs;
    uint64_t phys;
    uint32_t enqueue_index;
    uint8_t cycle;
};

struct erst_entry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed));

struct xhci_intr_regs {
    uint32_t iman;   // Interrupt Management
    uint32_t imod;   // Interrupt Moderation
    uint32_t erstsz; // Event Ring Segment Table Size
    uint32_t reserved;
    uint64_t erstba; // Event Ring Segment Table Base Address
    uint64_t erdp;   // Event Ring Dequeue Pointer
} __attribute__((packed));

#define EVENT_RING_SIZE 256
