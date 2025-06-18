#include <stdbool.h>
#include <stdint.h>
#pragma once

#define XHCI_DEVICE_TIMEOUT 1000
#define TRB_RING_SIZE 256

// NOTE: In scatter gathers, the first TRD must NOT point to a page-aligned
// boundary. Following TRDs must point to page-aligned boundaries.

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

    uint32_t root_hub_port : 8; /* Root hub port
                                 * number used to
                                 * access the USB
                                 * device */

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

    uint32_t dcs : 1; /* Dequeue cycle state - value of the xHC CCS
                       * (Consumer Cycle State) flag for the TRB
                       * referenced by the TR Dequeue pointer.
                       * '0' if `max_pstreams` > '0'
                       */

    uint32_t reserved4 : 3;
    uint32_t dequeue_ptr_lo : 28; /* Lower 28 bits of the dequeue pointer
                                   * MUST be aligned to 16 BYTE BOUNDARY
                                   */

    uint32_t dequeue_ptr_hi; /* Upper bits of the dequeue pointer
                              * MUST be aligned to 16 BYTE BOUNDARY
                              */

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
    struct xhci_ep_ctx ep_ctx[32];
} __attribute__((packed, aligned(64)));

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

struct xhci_trb {
    uint64_t parameter;
    uint32_t status;

    union {
        uint32_t control;
        struct {
            uint32_t cycle : 1;
            uint32_t ent : 1;
            uint32_t isp : 1;
            uint32_t ns : 1;
            uint32_t ch : 1;
            uint32_t ioc : 1; // Bit 5 (interrupt on completion)
            uint32_t idt : 1; // Bit 6 (immediate data)
            uint32_t reserved1 : 3;
            uint32_t trb_type : 6;
            uint32_t reserved2 : 16;
        };
    };
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_trb) == 0x10, "");

struct xhci_ring {
    struct xhci_trb *trbs;  // Virtual mapped TRB buffer
    uint64_t phys;          // Physical address of TRB buffer
    uint32_t enqueue_index; // Next TRB to fill
    uint8_t cycle;          // Cycle bit, toggles after ring wrap
    uint32_t size;          // Number of TRBs in ring (e.g., 256)
};

struct xhci_erst_entry {
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

struct xhci_slot {
    bool in_use;
    uint8_t port_num;
    struct xhci_ring ep_rings[31]; // EP 0 to 30
    uint64_t input_ctx_phys;
    uint64_t dev_ctx_phys;
    struct xhci_input_ctx *input_ctx;
    void *dev_ctx;
};

struct xhci_device {
    struct xhci_cap_regs *cap_regs;
    struct xhci_op_regs *op_regs;
    struct xhci_intr_regs *intr_regs; // Interrupt registers

    uint64_t *dcbaa; // Virtual address of DCBAA

    struct xhci_ring *event_ring;
    struct xhci_ring *cmd_ring;
    struct xhci_erst_entry *erst;
    uint64_t ring_count;
};

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func);
