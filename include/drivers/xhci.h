/* @title: xHCI */
#pragma once
#include <math/bit_range.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/locked_list.h>
#include <sync/semaphore.h>
#include <thread/defer.h>
struct usb_controller;
struct usb_packet;
struct xhci_request;
struct usb_endpoint;
struct pci_device;

#define XHCI_DEVICE_TIMEOUT 1000
#define TRB_RING_SIZE 256
#define XHCI_PORT_COUNT 64
#define XHCI_SLOT_COUNT 255

#define XHCI_INPUT_CTX_ADD_FLAGS ((1 << 0) | (1 << 1))

#define XHCI_SETUP_TRANSFER_TYPE_NONE 0
#define XHCI_SETUP_TRANSFER_TYPE_OUT 2
#define XHCI_SETUP_TRANSFER_TYPE_IN 3

#define XHCI_USBSTS_HCH 1        /* HC halted */
#define XHCI_USBSTS_HSE (1 << 2) /* host system error */
#define XHCI_USBSTS_EI (1 << 3)  /* event interrupt */
#define XHCI_USBSTS_PCD (1 << 4) /* port change detect */
#define XHCI_IMAN_MASK 0x2
#define XHCI_IMAN_INT_PENDING 0x1
#define XHCI_IMAN_INT_ENABLE 0x2

#define XHCI_ENDPOINT_TYPE_INVAL 0
#define XHCI_ENDPOINT_TYPE_ISOCH_OUT 1
#define XHCI_ENDPOINT_TYPE_BULK_OUT 2
#define XHCI_ENDPOINT_TYPE_INTERRUPT_OUT 3
#define XHCI_ENDPOINT_TYPE_CONTROL_BI 4
#define XHCI_ENDPOINT_TYPE_ISOCH_IN 5
#define XHCI_ENDPOINT_TYPE_BULK_IN 6
#define XHCI_ENDPOINT_TYPE_INTERRUPT_IN 7

// NOTE: In scatter gathers, the first TRD must NOT point to a page-aligned
// boundary. Following TRDs must point to page-aligned boundaries.

#define XHCI_EXT_CAP_ID_LEGACY_SUPPORT 1
#define XHCI_EXT_CAP_ID_USB 2

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
#define TRB_TYPE_NO_OP_COMMAND 0x8
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
#define TRB_TYPE_NO_OP_2_COMMAND 0x17

// Event TRBs
#define TRB_TYPE_TRANSFER_EVENT 0x20
#define TRB_TYPE_COMMAND_COMPLETION 0x21
#define TRB_TYPE_PORT_STATUS_CHANGE 0x22
#define TRB_TYPE_BANDWIDTH_REQUEST 0x23
#define TRB_TYPE_DOORBELL_EVENT 0x24
#define TRB_TYPE_HOST_CONTROLLER_EVENT 0x25
#define TRB_TYPE_DEVICE_NOTIFICATION 0x26
#define TRB_TYPE_MFINDEX_WRAP 0x27

#define TRB_FIELD(val, lo, hi) BIT_RANGE(val, lo, hi)

#define TRB_TYPE(ctrl) TRB_FIELD(ctrl, 10, 15)
#define TRB_SLOT(ctrl) TRB_FIELD(ctrl, 24, 31)
#define TRB_EP(ctrl) TRB_FIELD(ctrl, 16, 23)

#define TRB_CC(status) TRB_FIELD(status, 24, 31)
#define TRB_PORT(parameter) TRB_FIELD(parameter, 24, 31)

#define TRB_SET_TYPE(val) (((val) & 0x3F) << 10)
#define TRB_SET_CYCLE(val) (((val) & 1))
#define TRB_SET_INTERRUPTER_TARGET(target) ((target) >> 21)

#define TRB_CYCLE_BIT (1 << 0)
#define TRB_ENT_BIT (1 << 1) // Evaluate Next TRB
#define TRB_ISP_BIT (1 << 2) // Interrupt on Short Packet
#define TRB_NS_BIT (1 << 3)  // No Snoop
#define TRB_CH_BIT (1 << 4)  // Chain
#define TRB_IOC_BIT (1 << 5) // Interrupt On Completion
#define TRB_IDT_BIT (1 << 6) // Immediate Data
#define TRB_BEI_BIT (1 << 9) // Block Event Interrupt (ISO)
#define TRB_TOGGLE_CYCLE_BIT (1 << 1)
#define TRB_TYPE_SHIFT 10

#define TRB_SET_SLOT_ID(id) (((id) & 0xFF) << 24)

// Bit definitions for XHCI PORTSC register
#define PORTSC_CCS (1 << 0)           // Current Connect Status
#define PORTSC_PED (1 << 1)           // Port Enabled/Disabled
#define PORTSC_OCA (1 << 3)           // Over-Current Active
#define PORTSC_RESET (1 << 4)         // Port Reset
#define PORTSC_PR (1 << 4)            // Port Reset
#define PORTSC_PLSE (1 << 5)          // Port Link State Enable
#define PORTSC_PRES (1 << 6)          // Port Resume
#define PORTSC_PP (1 << 9)            // Port Power
#define PORTSC_SPEED_MASK (0xF << 10) // Bits 10–13: Port Speed
#define PORTSC_SPEED_SHIFT 10

#define PORTSC_PLS_SHIFT 5
#define PORTSC_PLS_MASK (0xF << 5)
#define PORTSC_LWS (1 << 16) // Link Write Strobe
#define PORTSC_CSC (1 << 17) // Connect Status Change
#define PORTSC_PEC (1 << 18) // Port Enable/Disable Change
#define PORTSC_WRC (1 << 19) // Warm Port Reset Change
#define PORTSC_OCC (1 << 20) // Over-current Change
#define PORTSC_PRC (1 << 21) // Port Reset Change
#define PORTSC_PLC (1 << 22) // Port Link State Change
#define PORTSC_CEC (1 << 23) // Port Config Error Change

#define PORTSC_PLS_POLLING 7
#define PORTSC_PLS_U0 0
#define PORTSC_PLS_U2 2
#define PORTSC_PLS_U3 3
#define PORTSC_PLS_RXDETECT 5

#define PORTSC_IND (1 << 24)     // Port Indicator Control
#define PORTSC_LWS_BIT (1 << 16) // Link Write Strobe
#define PORTSC_DR (1 << 30)      // Device Removable
#define PORTSC_WPR (1u << 31)    // Warm Port Reset

#define PORT_SPEED_FULL 1       // USB 1.1 Full Speed
#define PORT_SPEED_LOW 2        // USB 1.1 Low Speed
#define PORT_SPEED_HIGH 3       // USB 2.0 High Speed
#define PORT_SPEED_SUPER 4      // USB 3.0 SuperSpeed
#define PORT_SPEED_SUPER_PLUS 5 // USB 3.1 Gen2 (SuperSpeed+)

#define CC_SUCCESS 1
#define CC_DATA_BUFFER_ERROR 2
#define CC_BABBLE_DETECTED 3
#define CC_USB_TRANSACTION_ERROR 4
#define CC_TRB_ERROR 5
#define CC_STALL_ERROR 6
#define CC_RESOURCE_ERROR 7
#define CC_BANDWIDTH_ERROR 8
#define CC_NO_SLOTS_AVAILABLE 9
#define CC_INVALID_STREAM_TYPE 10
#define CC_SLOT_NOT_ENABLED_ERROR 11
#define CC_ENDPOINT_NOT_ENABLED 12
#define CC_SHORT_PACKET 13
#define CC_RING_UNDERRUN 14
#define CC_RING_OVERRUN 15
#define CC_VF_EVENT_RING_FULL_ERROR 16
#define CC_PARAMETER_ERROR 17
#define CC_BANDWIDTH_OVERRUN_ERROR 18
#define CC_CONTEXT_STATE_ERROR 19
#define CC_NO_PING_RESPONSE_ERROR 20
#define CC_EVENT_RING_FULL 21
#define CC_INCOMPATIBLE_DEVICE 22
#define CC_MISSED_SERVICE 23
#define CC_COMMAND_RING_STOPPED 24
#define CC_COMMAND_ABORTED 25
#define CC_STOPPED 26
#define CC_STOPPED_LEN_INVALID 27
#define CC_STOPPED_SHORT_PACKET 28
#define CC_MAX_EXIT_LATENCY_TOO_LARGE 29

#define xhci_info(string, ...) k_info("XHCI", K_INFO, string, ##__VA_ARGS__)
#define xhci_warn(string, ...) k_info("XHCI", K_WARN, string, ##__VA_ARGS__)
#define xhci_error(string, ...) k_info("XHCI", K_ERROR, string, ##__VA_ARGS__)

/*
 * Lifetime invariant:
 * - Slot refcount > 0 implies:
 *   - Slot is ENABLED or DISCONNECTING
 *   - Endpoint rings may exist
 * - When slot refcount reaches 0:
 *   - Disable Slot has completed
 *   - No TRBs may be issued
 *   - All memory may be freed
 */

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

struct xhci_port_regs {
    uint32_t portsc;   // Port Status and Control (offset 0x00)
    uint32_t portpmsc; // Port Power Management Status and Control (offset 0x04)
    uint32_t portli;   // Port Link Info (offset 0x08)
    uint32_t portct;   // Port Configuration Timeout (offset 0x0C)
};

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
static_assert_struct_size_eq(xhci_usbcmd, sizeof(uint32_t));

/* Page 444 */
struct xhci_slot_ctx {
    uint32_t route_string : 20; // Location in USB topology

    uint32_t speed : 4; // Speed of the device

    uint32_t reserved0 : 1;

    uint32_t mtt : 1; /* Set to '1' by software
                       * if this is a high speed hub that
                       * supports multiple TTs*/

    uint32_t hub : 1; /* Set to '1' by software
                       * if this is a USB hub, 0 if
                       * function */

    uint32_t context_entries : 5; /* Index of the last
                                   * valid endpoint context
                                   * within this structure */

    uint32_t max_exit_latency : 16; /* In microseconds,
                                     * worst case time it takes
                                     * to wake up all links
                                     * in path to device*/

    uint32_t root_hub_port : 8; /* Root hub port number used to
                                 * access the USB device */

    uint32_t num_ports : 8; /* If this is a hub,
                             * this is set by software to
                             * identify downstream ports
                             * supported by the hub */

    uint32_t parent_hub_slot_id : 8; /* Slot ID of the
                                      * parent high-speed hub */

    uint32_t parent_port_number : 8; /* Port number of the
                                      * parent high-speed hub */

    uint32_t parent_think_time : 2; /* Think time of the
                                     * parent high-speed hub */

    uint32_t reserved1 : 4;

    uint32_t interrupter_target : 10; /* Index of the interrupter to events
                                       * generated by this slot */

    uint32_t usb_device_address : 8; /* Address of USB device assigned by xHC */
    uint32_t reserved2 : 19;
    uint32_t slot_state : 5; /* 0 - disabled/enabled, 1 - default, 2 -
                              * addressed, 3 - configured, rest reserved*/
    uint32_t reserved3[4];
} __attribute__((packed));
static_assert_struct_size_eq(xhci_slot_ctx, 0x20);

struct xhci_ep_ctx { // Refer to page 450 of the XHCI specification

    /* DWORD 0 */
    uint32_t ep_state : 3; /* The current operational state of the endpoint
                            * 0 - Disabled (non-operational)
                            * 1 - Running
                            * 2 - Halted
                            * 3 - Stopped
                            * 4 - Error - SW can manipulate the transfer ring
                            * 5-7 - Reserved
                            */

    uint32_t reserved1 : 5;

    uint32_t mult : 2; /* Maximum number of bursts in an interval
                        * that this endpoint supports.
                        * Zero-based value, 0-3 is 1-4 bursts
                        * 0 for all endpoint types except SS Isochronous
                        */

    uint32_t max_pstreams : 5; /* Maximum Primary streams this endpoint
                                * supports. If this is '0', the TR
                                * dequeue pointer should point to a
                                * transfer ring.
                                * If this is > '0' the TR dequeue
                                * pointer points to a Primary Stream Context
                                * Array.
                                */

    uint32_t lsa : 1; /* Linear Stream Array, identifies how
                       * an ID stream is interpreted
                       * '1' disables Secondary Stream Arrays
                       *  Linear index into Primary Stream Array
                       *  Valid values are MaxPstreams 1 - 15
                       */

    uint32_t interval : 8; /* Period between consecutive requests
                            * to a USB endpoint, expressed in 125 microsecond
                            * increments. Interval of 0 means period of 125us.
                            * A value of 1 means a period of 250us, etc.
                            */

    uint32_t max_esit_payload_hi : 8; /* Max Endpoint Service Time
                                       * Interval Payload High
                                       * If LEC is '1' this is the high order
                                       * 8 bits of the max ESIT payload value.
                                       * If this is '0', then this is reserved
                                       */

    /* DWORD 1 */
    uint32_t reserved2 : 1;

    uint32_t error_count : 2; /* Two bit down count, identifying the number
                               * of consecutive USB-bus errors allowed
                               * while executing a TD.
                               */

    uint32_t ep_type : 3; /* Endpoint type
                           * 0 - Not valid
                           * 1 - Isochronous Out
                           * 2 - Bulk Out
                           * 3 - Interrupt Out
                           * 4 - Control Bidirectional
                           * 5 - Isochronous In
                           * 6 - Bulk In
                           * 7 - Interrupt In
                           */

    uint32_t reserved3 : 1;
    uint32_t host_initiate_disable : 1; /* The field affects Stream enabled
                                         * endpoints allowing the HI stream
                                         * selection feature to be disabled for
                                         * the endpoint. Setting to '1' disables
                                         * the HI stream selection feature. '0'
                                         * enables normal stream operation
                                         */

    uint32_t max_burst_size : 8; /* Maximum number of consecutive USB
                                  * transactions per scheduling opportunity.
                                  * 0-based, 0-15 means 1-16
                                  */

    uint32_t max_packet_size : 16; /* Max packet size in bytes that this
                                    * endpoint is capable of sending or
                                    * receiving when configured.
                                    */

    /* DWORD 2 */
    union {
        uint64_t dequeue_ptr_raw;
        struct {
            uint32_t dcs : 1; /* Dequeue cycle state - value of the xHC CCS
                               * (Consumer Cycle State) flag for the TRB
                               * referenced by the TR Dequeue pointer.
                               * '0' if `max_pstreams` > '0'
                               */

            uint32_t reserved4 : 3;

            uint64_t dequeue_ptr : 60; /* dequeue pointer
                                        * MUST be aligned to 16 BYTE BOUNDARY
                                        */
        };
    };

    /* DWORD 4 */
    uint32_t average_trb_length : 16; /* Average length of TRBs executed
                                       * by this endpoint. Must be > '0'
                                       */

    uint32_t max_esit_payload_lo : 16; /* Low order 16 bits of
                                        * max ESIT payload.
                                        * Represends the total
                                        * number of bytes
                                        * this endpoint will
                                        * transfer during an ESIT
                                        */
    uint32_t reserved5[3];
} __attribute__((packed));
static_assert_struct_size_eq(xhci_ep_ctx, 0x20);

struct xhci_input_ctrl_ctx { // Refer to page 461 of the XHCI specification

    uint32_t drop_flags; /* Single bitfields to identify which
                          * device context data structs
                          * should be disabled by command.
                          * '1' disables the respective EP CTX.
                          *
                          * First two bits reserved
                          */

    uint32_t add_flags; /* Single bitfields to identify
                         * which device CTX data structures
                         * should be evaluated or enabled
                         * by command. '1' enables the respective
                         * CTX
                         */

    uint32_t reserved[5];

    uint32_t config : 8; /* If CIC = '1' and CIE = '1', and this input CTX
                          * is associated with a Configure Endpoint command,
                          * this field contains the value
                          * of the Standard Configuration Descriptor
                          * bConfiguration field associated with this command.
                          * Otherwise, clear to '0'
                          */

    uint32_t interface_num : 8; /* If CIC = '1' and CIE = '1',
                                 * and this input CTX
                                 * is associated with a
                                 * Configure Endpoint
                                 * Command, and this command
                                 * was issued due to a
                                 * SET_INTERFACE request,
                                 * this contains the
                                 * Standard Interface
                                 * Descriptor bInterfaceNumber
                                 * field associated with the command.
                                 * Otherwise, clear to '0'
                                 */

    uint32_t alternate_setting : 8; /* If CIC = '1' and CIE = '1',
                                     * and this input CTX is associated
                                     * with a Configure Endpoint command,
                                     * and the command was issued due
                                     * to a SET_INTERFACE request,
                                     * then this field contains
                                     * the value of the Standard
                                     * Interface Descriptor bAlternateSetting
                                     * field associated with the
                                     * command. Otherwise, clear to '0'.
                                     */

    uint32_t reserved1 : 8;
} __attribute__((packed));
static_assert_struct_size_eq(xhci_input_ctrl_ctx, 0x20);

struct xhci_input_ctx { // Refer to page 460 of the XHCI Spec
    struct xhci_input_ctrl_ctx ctrl_ctx;
    struct xhci_slot_ctx slot_ctx;
    struct xhci_ep_ctx ep_ctx[31];
} __attribute__((packed));
static_assert_struct_size_eq(xhci_input_ctx, 0x420);

struct xhci_device_ctx {
    struct xhci_slot_ctx slot_ctx;
    struct xhci_ep_ctx ep_ctx[32]; // Endpoint 1–31 (ep0 separate)
} __attribute__((packed));

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
    uint32_t reserved3[241];
    struct xhci_port_regs regs[];
} __attribute__((packed));

struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));
static_assert_struct_size_eq(xhci_trb, 0x10);

struct xhci_ring {
    struct xhci_trb *trbs;  /* Virtual mapped TRB buffer */
    uint64_t phys;          /* Physical address of TRB buffer */
    uint32_t enqueue_index; /* Next TRB to fill */
    uint32_t dequeue_index; /* Point where controller sends back things */
    uint8_t cycle;          /* Cycle bit, toggles after ring wrap */
    uint32_t size;          /* Number of TRBs in ring */
    size_t outgoing;
};

struct xhci_erst_entry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed));

#define XHCI_ERDP_EHB_BIT (1 << 3)

/* Page 424 */
struct xhci_interrupter_regs {
    uint32_t iman;   /* Interrupt management */
    uint32_t imod;   /* Interrupt moderation */
    uint32_t erstsz; /* Event Ring Segment Table Size */
    uint32_t reserved;
    uint64_t erstba; /* Event Ring Segment Table Base Address */
    uint64_t erdp;

} __attribute__((packed));

struct xhci_dcbaa { // Device context base address array - check page 441
    uint64_t ptrs[256];
} __attribute__((aligned(64)));

struct xhci_ext_cap {
    uint8_t cap_id;
    uint8_t next;
    uint16_t cap_specific;
};

enum xhci_slot_state {
    XHCI_SLOT_STATE_UNDEF,
    XHCI_SLOT_STATE_ENABLED,
    XHCI_SLOT_STATE_DISCONNECTING,
    XHCI_SLOT_STATE_DISCONNECTED,
};

static inline const char *xhci_slot_state_str(enum xhci_slot_state s) {
    switch (s) {
    case XHCI_SLOT_STATE_UNDEF: return "SLOT UNDEF";
    case XHCI_SLOT_STATE_ENABLED: return "SLOT ENABLED";
    case XHCI_SLOT_STATE_DISCONNECTING: return "SLOT DISCONNECTING";
    case XHCI_SLOT_STATE_DISCONNECTED: return "SLOT DISCONNECTED";
    default: return "SLOT UNKNOWN";
    }
}

enum xhci_port_state {
    XHCI_PORT_STATE_UNDEF,
    XHCI_PORT_STATE_CONNECTING,
    XHCI_PORT_STATE_CONNECTED,
    XHCI_PORT_STATE_DISCONNECTING,
    XHCI_PORT_STATE_DISCONNECTED,
};

static inline const char *xhci_port_state_str(enum xhci_port_state s) {
    switch (s) {
    case XHCI_PORT_STATE_UNDEF: return "PORT UNDEF";
    case XHCI_PORT_STATE_CONNECTING: return "PORT CONNECTING";
    case XHCI_PORT_STATE_CONNECTED: return "PORT CONNECTED";
    case XHCI_PORT_STATE_DISCONNECTING: return "PORT DISCONNECTING";
    case XHCI_PORT_STATE_DISCONNECTED: return "PORT DISCONNECTED";
    default: return "PORT UNKNOWN";
    }
}

struct xhci_slot {
    _Atomic enum xhci_slot_state state;
    struct xhci_device *dev;
    struct xhci_ring *ep_rings[32];
    uint8_t slot_id; /* Just so everyone knows what slot we are */

    /* As soon as this drops to zero we clear the ep_rings */
    refcount_t refcount;
    struct xhci_port *port;
    struct usb_device *udev;
};

/* protected by xhci_device->lock */
struct xhci_port {
    uint8_t port_id;
    struct xhci_slot *slot;
    uint8_t speed;
    bool usb3;
    uint64_t generation;
    enum xhci_port_state state;
    struct xhci_device *dev;
};

enum xhci_request_status {
    /* The first 3 statuses indicate list state.
     *
     * OUTGOING requests have a TRB, and have rang the doorbell.
     *
     * WAITING requests cannot be currently satisfied because the
     * command ring is full.
     *
     * PROCESSED requests have been handled by the ISR with a success state.
     * Technically they run in the worker thread but the status is set from ISR.
     *
     * DISCONNECT requests occur when a port is disconnected and the request
     * no longer goes anywhere (because the port is gone)
     */
    XHCI_REQUEST_OUTGOING,
    XHCI_REQUEST_WAITING,
    XHCI_REQUEST_PROCESSED,
    XHCI_REQUEST_MAX,
    XHCI_REQUEST_CANCELLED,
    XHCI_REQUEST_DISCONNECT,
    XHCI_REQUEST_ERR,
};

struct xhci_device {
    uint8_t irq; /* What IRQ line have we routed to this? */
    struct pci_device *pci;
    struct xhci_input_ctx *input_ctx;
    struct xhci_cap_regs *cap_regs;          /* Capability registers */
    struct xhci_op_regs *op_regs;            /* Operational registers */
    struct xhci_interrupter_regs *intr_regs; /* Interrupter registers */

    struct xhci_dcbaa *dcbaa;

    struct xhci_ring *event_ring;
    struct xhci_ring *cmd_ring;
    struct xhci_erst_entry *erst;

    struct xhci_port_regs *port_regs;
    uint64_t ports;
    struct xhci_slot slots[XHCI_SLOT_COUNT];
    struct xhci_port port_info[XHCI_PORT_COUNT];

    struct list_head requests[XHCI_REQUEST_MAX];

    size_t num_devices;
    struct list_head devices;
    struct spinlock lock;
    struct semaphore sem;
    struct semaphore port_disconnect;
    struct semaphore port_connect;
    struct usb_controller *controller;
    atomic_bool worker_waiting;
};

struct xhci_command {
    struct xhci_ring *ring; /* what ring? */
    struct xhci_slot *slot;
    uint32_t ep_id;

    size_t num_trbs;
    void (*emit)(struct xhci_command *cmd, struct xhci_ring *ring);

    struct xhci_request *request; /* associated request */
    void *private;
};

struct xhci_request {
    /* Tied back to the USB request */
    struct usb_request *urb;
    struct xhci_command *command;

    volatile uint8_t port_reset;
    volatile uint8_t slot_reset;
    struct xhci_trb *last_trb;
    uint64_t trb_phys;
    uint8_t port; /* What port is this for? Used to match
                   * requests on disconnect */

    /* the raw completion code for this request */
    volatile uint8_t completion_code;
    volatile uint64_t return_parameter;
    volatile uint32_t return_status;
    volatile uint32_t return_control;
    uint64_t generation;

    volatile enum xhci_request_status status;

    struct list_head list;
    void (*callback)(struct xhci_device *, struct xhci_request *);
    void *private;
};

struct xhci_return {
    uint32_t control;
    uint32_t status;
};

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func, struct pci_device *dev);
