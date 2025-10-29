#include <stdbool.h>
#include <stdint.h>
#pragma once

#define XHCI_DEVICE_TIMEOUT 1000
#define TRB_RING_SIZE 256

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

// Control field helpers
#define TRB_GET_TYPE(ctrl) (((ctrl) >> 10) & 0x3F)
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
#define TRB_TYPE_SHIFT 10

#define TRB_SET_SLOT_ID(id) (((id) & 0xFF) << 24)
#define TRB_GET_SLOT_ID(ctrl) (((ctrl) >> 24) & 0xFF)

// Bit definitions for XHCI PORTSC register
#define PORTSC_CCS (1 << 0)           // Current Connect Status
#define PORTSC_PED (1 << 1)           // Port Enabled/Disabled
#define PORTSC_OCA (1 << 3)           // Over-Current Active
#define PORTSC_RESET (1 << 4)         // Port Reset
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

#define CC_SUCCESS 0x01
#define CC_DATA_BUFFER_ERROR 0x02
#define CC_BABBLE_DETECTED 0x03
#define CC_USB_TRANSACTION_ERROR 0x04
#define CC_TRB_ERROR 0x05
#define CC_STALL_ERROR 0x06
#define CC_RESOURCE_ERROR 0x07
#define CC_BANDWIDTH_ERROR 0x08
#define CC_NO_SLOTS_AVAILABLE 0x09
#define CC_INVALID_STREAM_TYPE 0x0A
#define CC_SLOT_NOT_ENABLED_ERROR 0x0B

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

_Static_assert(sizeof(struct xhci_usbcmd) == sizeof(uint32_t), "");

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
_Static_assert(sizeof(struct xhci_slot_ctx) == 0x20, "");

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
_Static_assert(sizeof(struct xhci_ep_ctx) == 0x20, "");

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
_Static_assert(sizeof(struct xhci_input_ctrl_ctx) == 0x20, "");

struct xhci_input_ctx { // Refer to page 460 of the XHCI Spec
    struct xhci_input_ctrl_ctx ctrl_ctx;
    struct xhci_slot_ctx slot_ctx;
    struct xhci_ep_ctx ep_ctx[31];
} __attribute__((packed));
_Static_assert(sizeof(struct xhci_input_ctx) == 0x420, "");

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

_Static_assert(sizeof(struct xhci_trb) == 0x10, "");

struct xhci_ring {
    struct xhci_trb *trbs;  /* Virtual mapped TRB buffer */
    uint64_t phys;          /* Physical address of TRB buffer */
    uint32_t enqueue_index; /* Next TRB to fill */
    uint32_t dequeue_index; /* Point where controller sends back things */
    uint8_t cycle;          /* Cycle bit, toggles after ring wrap */
    uint32_t size;          /* Number of TRBs in ring */
};

struct xhci_erst_entry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed));

struct xhci_erdp {
    union {
        uint64_t raw;
        struct {
            uint64_t desi : 3; /* Dequeue ERST Segment Index */
            uint64_t ehb : 1;  /* Event Handler Busy */
            uint64_t event_ring_pointer : 60;
        };
    };
} __attribute__((packed));
_Static_assert(sizeof(struct xhci_erdp) == 8, "");

#define XHCI_INTERRUPTER_INTERRUPT_PENDING 1
#define XHCI_INTERRUPTER_INTERRUPT_ENABLE (1 << 1)

/* Page 424 */
struct xhci_interrupter_regs {
    uint32_t iman;   /* Interrupt management */
    uint32_t imod;   /* Interrupt moderation */
    uint32_t erstsz; /* Event Ring Segment Table Size */
    uint32_t reserved;
    uint64_t erstba; /* Event Ring Segment Table Base Address */
    union {
        struct xhci_erdp erdp;
        uint64_t erdp_raw;
    };

} __attribute__((packed));

struct xhci_port_info {
    bool device_connected;
    uint8_t speed;
    uint8_t slot_id;
    bool usb3;
    struct xhci_ring *ep_rings[32];
};

struct xhci_dcbaa { // Device context base address array - check page 441
    uint64_t ptrs[256];
} __attribute__((aligned(64)));

struct xhci_ext_cap {
    uint8_t cap_id;
    uint8_t next;
    uint16_t cap_specific;
};

struct xhci_device {
    uint8_t irq;
    struct pci_device *pci;
    struct xhci_input_ctx *input_ctx;
    struct xhci_cap_regs *cap_regs;
    struct xhci_op_regs *op_regs;
    struct xhci_interrupter_regs *intr_regs; // Interrupt registers

    struct xhci_dcbaa *dcbaa; // Virtual address of DCBAA

    struct xhci_ring *event_ring;
    struct xhci_ring *cmd_ring;
    struct xhci_erst_entry *erst;
    struct xhci_port_regs *port_regs;
    uint64_t ports;
    struct xhci_port_info port_info[64];

    uint64_t num_devices;
    struct usb_device **devices;
};

/* I don't want to cause even longer compile times
 * by including the ever-growing USB header in here */
struct usb_controller;
struct usb_packet;
struct usb_endpoint;
struct pci_device;

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func, struct pci_device *dev);
void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func);
struct xhci_device *xhci_device_create(void *mmio);
bool xhci_controller_stop(struct xhci_device *dev);
bool xhci_controller_reset(struct xhci_device *dev);
bool xhci_controller_start(struct xhci_device *dev);
void xhci_controller_enable_ints(struct xhci_device *dev);
void xhci_setup_event_ring(struct xhci_device *dev);
void xhci_setup_command_ring(struct xhci_device *dev);

bool xhci_submit_interrupt_transfer(struct usb_device *dev,
                                    struct usb_packet *packet);

void xhci_send_command(struct xhci_device *dev, uint64_t parameter,
                       uint32_t control);

uint64_t xhci_wait_for_response(struct xhci_device *dev);
bool xhci_wait_for_transfer_event(struct xhci_device *dev, uint8_t slot_id);
uint8_t xhci_enable_slot(struct xhci_device *dev);
void xhci_parse_ext_caps(struct xhci_device *dev);
bool xhci_reset_port(struct xhci_device *dev, uint32_t port_index);
void xhci_detect_usb3_ports(struct xhci_device *dev);

#define xhci_info(string, ...) k_info("XHCI", K_INFO, string, ##__VA_ARGS__)
#define xhci_warn(string, ...) k_info("XHCI", K_WARN, string, ##__VA_ARGS__)
#define xhci_error(string, ...) k_info("XHCI", K_ERROR, string, ##__VA_ARGS__)
