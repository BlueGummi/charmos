#include <stdint.h>
#pragma once

// TRB Types (bits 10–15 in control word)
#define TRB_TYPE_RESERVED 0x00
#define TRB_TYPE_NORMAL 0x01
#define TRB_TYPE_SETUP_STAGE 0x02
#define TRB_TYPE_DATA_STAGE 0x03
#define TRB_TYPE_STATUS_STAGE 0x04
#define TRB_TYPE_ISOCH 0x05
#define TRB_TYPE_LINK 0x06
#define TRB_TYPE_EVENT_DATA 0x07
#define TRB_TYPE_NO_OP 0x08

// Command TRBs
#define TRB_TYPE_ENABLE_SLOT 0x09
#define TRB_TYPE_DISABLE_SLOT 0x0A
#define TRB_TYPE_ADDRESS_DEVICE 0x0B
#define TRB_TYPE_CONFIGURE_ENDPOINT 0x0C
#define TRB_TYPE_EVALUATE_CONTEXT 0x0D
#define TRB_TYPE_RESET_ENDPOINT 0x0E
#define TRB_TYPE_STOP_ENDPOINT 0x0F
#define TRB_TYPE_SET_TR_DEQUEUE_POINTER 0x10
#define TRB_TYPE_RESET_DEVICE 0x11
#define TRB_TYPE_FORCE_EVENT 0x12
#define TRB_TYPE_NEGOTIATE_BW 0x13
#define TRB_TYPE_SET_LATENCY_TOLERANCE 0x14
#define TRB_TYPE_GET_PORT_BANDWIDTH 0x15
#define TRB_TYPE_FORCE_HEADER 0x16
#define TRB_TYPE_NO_OP_COMMAND 0x17

// Event TRBs
#define TRB_TYPE_TRANSFER_EVENT 0x20
#define TRB_TYPE_COMMAND_COMPLETION 0x21
#define TRB_TYPE_PORT_STATUS_CHANGE 0x22
#define TRB_TYPE_BANDWIDTH_REQUEST 0x23
#define TRB_TYPE_DOORBELL_EVENT 0x24
#define TRB_TYPE_HOST_CONTROLLER_EVENT 0x25
#define TRB_TYPE_DEVICE_NOTIFICATION 0x26
#define TRB_TYPE_MFINDEX_WRAP 0x27

// Control field helpers ('control' is the 32-bit TRB control word)
#define TRB_GET_TYPE(ctrl) (((ctrl) >> 10) & 0x3F)
#define TRB_SET_TYPE(val) (((val) & 0x3F) << 10)

#define TRB_CYCLE_BIT (1 << 0)
#define TRB_ENT_BIT (1 << 1) // Evaluate Next TRB
#define TRB_ISP_BIT (1 << 2) // Interrupt on Short Packet
#define TRB_NS_BIT (1 << 3)  // No Snoop
#define TRB_CH_BIT (1 << 4)  // Chain
#define TRB_IOC_BIT (1 << 5) // Interrupt On Completion
#define TRB_IDT_BIT (1 << 6) // Immediate Data
#define TRB_BEI_BIT (1 << 9) // Block Event Interrupt (ISO)

#define TRB_SET_SLOT_ID(id) (((id) & 0xFF) << 24)
#define TRB_GET_SLOT_ID(ctrl) (((ctrl) >> 24) & 0xFF)

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

struct xhci_usbcmd {
    union {
        uint32_t raw;
        struct {
            uint32_t run_stop : 1;
            uint32_t host_controller_reset : 1;
            uint32_t interrupter_enable : 1;
            uint32_t host_system_error_en : 1;
            uint32_t reserved0 : 3;
            uint32_t light_host_controller_reset : 1;
            uint32_t controller_save_state : 1;
            uint32_t controller_restore_state : 1;

            uint32_t enable_wrap_event : 1;
            uint32_t enable_u3_mf_index : 1;
            uint32_t reserved1 : 1;

            uint32_t cem_enable : 1;
            uint32_t extended_tbc_enable : 1;

            uint32_t extended_tbc_trb_status_enable : 1;

            uint32_t vtio_enable : 1;

            uint32_t reserved2 : 15;
        };
    };
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_usbcmd) == sizeof(uint32_t), "");

// 5.4: XHCI Operational Registers
struct xhci_op_regs {
    struct xhci_usbcmd usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint32_t reserved[2];
    uint32_t dnctrl;
    uint64_t crcr;
    uint32_t reserved2[4];
    uint64_t dcbaap;
    uint32_t config;
} __attribute__((packed));

struct xhci_trb { // Transfer Request Block
    uint64_t parameter;
    uint32_t status;
    union {
        struct {
            uint32_t cycle : 1;
            uint32_t ent : 1;
            uint32_t isp : 1;
            uint32_t ns : 1;
            uint32_t chain : 1;
            uint32_t ioc : 1;
            uint32_t idt : 1;
            uint32_t reserved : 2;
            uint32_t trb_type : 6;
            uint32_t reserved2 : 16;
        };
        uint32_t control;
    };
};

struct xhci_ring {
    struct xhci_trb *trbs;  // Virtual mapped TRB buffer
    uint64_t phys;          // Physical address of TRB buffer
    uint32_t enqueue_index; // Next TRB to fill
    uint8_t cycle;          // Cycle bit, toggles after ring wrap
    uint32_t size;          // Number of TRBs in ring (e.g., 256)
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
