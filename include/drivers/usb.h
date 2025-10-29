#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defines generic USB constants, functions, and structures */

/* All page numbers are for the USB 2.0 Specification */

/* Page 279 - Descriptor types */
#define USB_DESC_TYPE_DEVICE 1
#define USB_DESC_TYPE_CONFIG 2
#define USB_DESC_TYPE_STRING 3
#define USB_DESC_TYPE_INTERFACE 4
#define USB_DESC_TYPE_ENDPOINT 5
#define USB_DESC_TYPE_DEV_QUALIFIER 6
#define USB_DESC_TYPE_OTHER_SPEED_CONFIG 7
#define USB_DESC_TYPE_INTERFACE_POWER 8

#define USB_DESC_TYPE_SHIFT 8

/* Request bitmap field definitions */
#define USB_REQUEST_TRANS_HTD 0
#define USB_REQUEST_TRANS_DTH 1

#define USB_REQUEST_TYPE_STANDARD 0
#define USB_REQUEST_TYPE_CLASS 1
#define USB_REQUEST_TYPE_VENDOR 2

#define USB_REQUEST_RECIPIENT_DEVICE 0
#define USB_REQUEST_RECIPIENT_INTERFACE 1
#define USB_REQUEST_RECIPIENT_ENDPOINT 2
#define USB_REQUEST_RECIPIENT_OTHER 3

#define USB_REQUEST_TRANSFER_SHIFT 7
#define USB_REQUEST_TRANSFER_MASK 1

#define USB_REQUEST_TYPE_SHIFT 5
#define USB_REQUEST_TYPE_MASK 3

#define USB_REQUEST_RECIPIENT_MASK 0x1F

#define USB_REQUEST_TRANSFER(byte) ((byte >> 7) & 1)
#define USB_REQUEST_TYPE(byte) ((byte >> 5) & 3)
#define USB_REQUEST_RECIPIENT(byte) (byte & 0x1F)

/* Config bitmap definitions */
#define USB_CONFIG_SELF_POWERED (1 << 7)
#define USB_CONFIG_REMOTE_WAKEUP (1 << 6)

/* Endpoint address bitmap definitions */
#define USB_ENDPOINT_ADDR_EP_DIRECTION_OUT 0
#define USB_ENDPOINT_ADDR_EP_DIRECTION_IN 1
#define USB_ENDPOINT_ADDR_EP_NUM(byte) (byte & 0xF)
#define USB_ENDPOINT_ADDR_EP_DIRECTION(byte) ((byte >> 7) & 1)

/* Endpoint attribute bitmap definitions */
#define USB_ENDPOINT_ATTR_TRANS_TYPE_CONTROL 0
#define USB_ENDPOINT_ATTR_TRANS_TYPE_ISOCHRONOUS 1
#define USB_ENDPOINT_ATTR_TRANS_TYPE_BULK 2
#define USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT 3

#define USB_ENDPOINT_ATTR_SYNC_TYPE_NO_SYNC 0
#define USB_ENDPOINT_ATTR_SYNC_TYPE_ASYNC 1
#define USB_ENDPOINT_ATTR_SYNC_TYPE_ADAPTIVE 2
#define USB_ENDPOINT_ATTR_SYNC_TYPE_SYNC 3

#define USB_ENDPOINT_ATTR_USAGE_TYPE_DATA 0
#define USB_ENDPOINT_ATTR_USAGE_TYPE_FEEDBACK 1
#define USB_ENDPOINT_ATTR_USAGE_TYPE_IMPLICIT_FEEDBACK_DATA 2

#define USB_ENDPOINT_ATTR_TRANS_TYPE(byte) (byte & 3)
#define USB_ENDPOINT_ATTR_SYNC_TYPE(byte) ((byte >> 2) & 3)
#define USB_ENDPOINT_ATTR_USAGE_TYPE(byte) ((byte >> 4) & 3)

/* Endpoint max packet size bitmap definitions */
#define USB_ENDPOINT_MAX_PACKET_SIZE_SIZE(byte) (byte & 0x3FF)
#define USB_ENDPOINT_MAX_PACKET_SIZE_TRANSACT_OPP(byte) ((byte >> 11) & 3)

#define USB_ENDPOINT_MAX_PACKET_SIZE_TRANSACT_OPP_NUM(num) (num + 1)

/* Endpoint interval conversion definitions */
#define USB_ENDPOINT_INTERVAL_TO_INTEGER_LOW_SPEED(num) (num)
#define USB_ENDPOINT_INTEGER_TO_INTERVAL_LOW_SPEED(num) (num)

#define USB_ENDPOINT_INTERVAL_TO_INTEGER_HIGH_SPEED(num) (2 << (num - 2))

/* Subclass codes are not defined in a centralized place. Add them as needed */
#define USB_CLASS_AUDIO 0x1
/* Audio subclasses go here */

#define USB_CLASS_COMMS 0x2
/* Comms subclasses go here */

#define USB_CLASS_HID 0x3

#define USB_SUBCLASS_HID_NONE 0x0
#define USB_SUBCLASS_HID_BOOT_INTERFACE 0x1
/* 2 - 225 reserved for HID */

#define USB_PROTOCOL_HID_NONE 0x0
#define USB_PROTOCOL_HID_KEYBOARD 0x1
#define USB_PROTOCOL_HID_MOUSE 0x2
/* 3 - 225 reserved */

#define USB_CLASS_PHYSICAL 0x5
#define USB_CLASS_IMAGE 0x6
#define USB_CLASS_PRINTER 0x7
#define USB_CLASS_MASS_STORAGE 0x8
#define USB_CLASS_HUB 0x9
#define USB_CLASS_DATA 0xA
#define USB_CLASS_SMART_CARD 0xB
#define USB_CLASS_CONTENT_SECURITY 0xD
#define USB_CLASS_VIDEO 0xE
#define USB_CLASS_PERSONAL_HEALTHCARE 0xF
#define USB_CLASS_AUDIO_VIDEO 0x10
#define USB_CLASS_BILLBOARD 0x11
#define USB_CLASS_USB_TYPE_C 0x12
#define USB_CLASS_BULK_DISPLAY 0x13
#define USB_CLASS_MCTP_OVER_USB 0x14
#define USB_CLASS_I3C_DEVICE 0x3C
#define USB_CLASS_DIAGNOSTIC_DEVICE 0xDC
#define USB_CLASS_WIRELESS 0xE0
#define USB_CLASS_MISC 0xEF
#define USB_CLASS_APPLICATION_SPECIFIC 0xFE
#define USB_CLASS_VENDOR_SPECIFIC 0xFF

/* Request codes */
enum usb_rq_code : uint8_t {
    USB_RQ_CODE_GET_STATUS = 0,    /* Page 282 */
    USB_RQ_CODE_CLEAR_FEATURE = 1, /* Page 280 */
    USB_RQ_CODE_SET_FEATURE = 3,   /* Page 286 */
    USB_RQ_CODE_SET_ADDR = 5,      /* Page 284 */

    USB_RQ_CODE_GET_DESCRIPTOR = 6, /* Page 281 */
    USB_RQ_CODE_SET_DESCRIPTOR = 7, /* Page 285 */

    USB_RQ_CODE_GET_CONFIG = 8, /* Page 281 */
    USB_RQ_CODE_SET_CONFIG = 9, /* Page 285 */

    USB_RQ_CODE_GET_INTERFACE = 10, /* Page 282 */
    USB_RQ_CODE_SET_INTERFACE = 11, /* Page 287 */

    USB_RQ_CODE_SYNCH_FRAME = 12, /* Page 288 */
};

static inline const char *usb_rq_code_str(const enum usb_rq_code code) {
    switch (code) {
    case USB_RQ_CODE_GET_STATUS: return "GET_STATUS";
    case USB_RQ_CODE_CLEAR_FEATURE: return "CLEAR_FEATURE";
    case USB_RQ_CODE_SET_FEATURE: return "SET_FEATURE";
    case USB_RQ_CODE_SET_ADDR: return "SET_ADDR";
    case USB_RQ_CODE_GET_DESCRIPTOR: return "GET_DESCRIPTOR";
    case USB_RQ_CODE_SET_DESCRIPTOR: return "SET_DESCRIPTOR";
    case USB_RQ_CODE_GET_CONFIG: return "GET_CONFIG";
    case USB_RQ_CODE_SET_CONFIG: return "SET_CONFIG";
    case USB_RQ_CODE_GET_INTERFACE: return "GET_INTERFACE";
    case USB_RQ_CODE_SET_INTERFACE: return "SET_INTERFACE";
    case USB_RQ_CODE_SYNCH_FRAME: return "SYNCH_FRAME";
    default: return "UNKNOWN_CODE";
    }
}

enum usb_controller_type {
    USB_CONTROLLER_UHCI,
    USB_CONTROLLER_EHCI,
    USB_CONTROLLER_XHCI,
};

struct usb_setup_packet {        /* Refer to page 276 */
    uint8_t bitmap_request_type; /* Characteristics */
    enum usb_rq_code request;    /* Specific request */
    uint16_t value;              /* Varies according to request */

    union {
        uint16_t index;
        uint16_t offset;
    };

    uint16_t length; /* Number of bytes if there is a data stage */

} __attribute__((packed));
_Static_assert(sizeof(struct usb_setup_packet) == 8, "");

struct usb_device_descriptor { /* Refer to page 290 */
    uint8_t length;
    uint8_t type;
    uint16_t usb_num_bcd; /* USB number in binary coded
                           * decimal. 2.10 is 210H */

    uint8_t class;    /* Class code */
    uint8_t subclass; /* Subclass code */
    uint8_t protocol;
    uint8_t max_packet_size; /* Only 8, 16, 32, or 64 are valid */

    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_num_bcd; /* Device number in binary coded decimal */

    uint8_t manufacturer; /* Index of string desc. describing manufacturer */
    uint8_t product;      /* Index of string desc. describing product */
    uint8_t serial_num;   /* Index of string desc. describing serial number */

    uint8_t num_configs; /* Number of possible configurations */
} __attribute__((packed));
_Static_assert(sizeof(struct usb_device_descriptor) == 18, "");

struct usb_interface_descriptor { /* Page 296 */
    uint8_t length;
    uint8_t type;
    uint8_t interface_number; /* zero-based value ident. the index
                               * in the array of concurrent interfaces */

    uint8_t alternate_setting; /* Select this alternate
                                * setting for the interface */

    uint8_t num_endpoints; /* Number of endpoings excluding EP0 */
    uint8_t class;         /* Interface class code */
    uint8_t subclass;      /* Interface subclass code */

    uint8_t protocol; /* Interface protocol code qualified by `class` and
                         `subclass` */

    uint8_t interface; /* Index of string desc. describing this interface */

} __attribute__((packed));
_Static_assert(sizeof(struct usb_interface_descriptor) == 9, "");

struct usb_config_descriptor { /* Page 293 */
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length; /* Total length of data for this config */
    uint8_t num_interfaces;
    uint8_t configuration_value; /* Value to use as argument
                                  * to SetConfig() request */

    uint8_t configuration; /* Index of string desc. describing this config */
    uint8_t bitmap_attributes; /* Bits 7 and 0..4 are reserved */

    uint8_t max_power; /* Max power of USB device in milliamps */
} __attribute__((packed));
_Static_assert(sizeof(struct usb_config_descriptor) == 9, "");

struct usb_endpoint_descriptor { /* Page 297 */
    uint8_t length;
    uint8_t type;
    uint8_t address;    /* 3..0 is EP number, 7 is direction */
    uint8_t attributes; /* Refer to definitions */

    uint16_t max_packet_size; /* Max packet size as a bitmap */

    uint8_t interval; /* Interval for polling this EP for data transfer */
} __attribute__((packed));
_Static_assert(sizeof(struct usb_endpoint_descriptor) == 7, "");

struct usb_endpoint {
    struct usb_endpoint_descriptor *desc;

    /* Putting these here so code won't look like
     * ep->desc.type, I think that looks bad */
    uint8_t type;
    uint8_t number;
    bool in; /* true - in, false - out */
    uint8_t *transfer_buffer;
    uint16_t transfer_len;
    uint16_t max_packet_size;
    uint8_t address;
    uint8_t attributes;
    uint8_t interval;

    void *hc_data;
};

struct usb_controller;

enum usb_transfer_type {
    USB_TRANSFER_CONTROL,
    USB_TRANSFER_BULK,
    USB_TRANSFER_INTERRUPT,
};

static inline const char *
usb_transfer_type_str(const enum usb_transfer_type type) {
    switch (type) {
    case USB_TRANSFER_CONTROL: return "USB TRANSFER CONTROL";
    case USB_TRANSFER_BULK: return "USB TRANSFER BULK";
    case USB_TRANSFER_INTERRUPT: return "USB TRANSFER INTERRUPT";
    default: return "UNKNOWN";
    }
}

struct usb_packet {
    struct usb_endpoint *ep;

    enum usb_transfer_type type;

    struct usb_setup_packet *setup;
    void *data;
    size_t length;
    bool direction_in;

    void (*completion_cb)(void *ctx, bool success);
    void *context;
};

struct usb_device;
struct usb_controller_ops {
    bool (*submit_control_transfer)(struct usb_device *dev,
                                    struct usb_packet *pkt);

    bool (*submit_bulk_transfer)(struct usb_device *dev,
                                 struct usb_packet *pkt);

    bool (*submit_interrupt_transfer)(struct usb_device *dev,
                                      struct usb_packet *pkt);

    bool (*reset_port)(struct usb_device *dev);

    bool (*setup_interrupt_endpoint)(
        struct usb_controller *ctrl, uint8_t port, struct usb_endpoint *ep,
        void (*callback)(void *ctx, uint8_t *data, size_t len), void *ctx);
};

struct usb_controller { /* Generic USB controller */
    enum usb_controller_type type;
    struct usb_controller_ops ops;
    void *driver_data;
};

struct usb_driver {
    const char *name;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;

    bool (*probe)(struct usb_device *dev);      /* Attach and set up */
    void (*disconnect)(struct usb_device *dev); /* Clean up and unplug */
} __attribute__((aligned(64)));
extern struct usb_driver __skernel_usb_drivers[];
extern struct usb_driver __ekernel_usb_drivers[];

#define REGISTER_USB_DRIVER(n, cc, sc, proto, probe_fn, disconnect_fn)         \
    static struct usb_driver usb_driver_##n                                    \
        __attribute__((section(".kernel_usb_drivers"), used)) = {              \
            .name = #n,                                                        \
            .class_code = cc,                                                  \
            .subclass = sc,                                                    \
            .protocol = proto,                                                 \
            .probe = probe_fn,                                                 \
            .disconnect = disconnect_fn};

struct usb_device {
    uint8_t address;
    uint8_t speed;
    uint8_t port;    /* Port number on the root hub */
    uint8_t slot_id; /* Used for xHCI */
    uint8_t max_packet_size;

    struct usb_device_descriptor *descriptor;
    struct usb_config_descriptor config;

    struct usb_endpoint **endpoints; /* List of endpoints */
    uint8_t num_endpoints;

    struct usb_controller *host;

    struct usb_interface_descriptor **interfaces; /* List of interfaces */
    uint8_t num_interfaces;

    struct usb_driver *driver; /* Attached driver */

    void *driver_private;

    bool configured;
};

bool usb_get_string_descriptor(struct usb_device *dev, uint8_t string_idx,
                               char *out, size_t max_len);
void usb_get_device_descriptor(struct usb_device *dev);
bool usb_parse_config_descriptor(struct usb_device *dev);
bool usb_set_configuration(struct usb_device *dev);

void usb_try_bind_driver(struct usb_device *dev);
uint8_t usb_construct_rq_bitmap(uint8_t transfer, uint8_t type, uint8_t recip);

static inline uint8_t get_ep_index(struct usb_endpoint *ep) {
    return (ep->number * 2) + (ep->in ? 1 : 0);
}

#define usb_info(string, ...) k_info("USB", K_INFO, string, ##__VA_ARGS__)
#define usb_warn(string, ...) k_info("USB", K_WARN, string, ##__VA_ARGS__)
#define usb_error(string, ...) k_info("USB", K_ERROR, string, ##__VA_ARGS__)
